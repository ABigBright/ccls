// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "clang_complete.hh"
#include "fuzzy_match.hh"
#include "include_complete.hh"
#include "log.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "working_files.hh"

#include <clang/Sema/CodeCompleteConsumer.h>
#include <clang/Sema/Sema.h>

#include <regex>

namespace ccls {
using namespace clang;
using namespace llvm;

MAKE_REFLECT_TYPE_PROXY(InsertTextFormat);
MAKE_REFLECT_TYPE_PROXY(CompletionItemKind);
MAKE_REFLECT_STRUCT(CompletionItem, label, kind, detail, documentation,
                    sortText, filterText, insertText, insertTextFormat,
                    textEdit, additionalTextEdits);

namespace {
struct CompletionList {
  bool isIncomplete = false;
  std::vector<CompletionItem> items;
};
MAKE_REFLECT_STRUCT(CompletionList, isIncomplete, items);

void DecorateIncludePaths(const std::smatch &match,
                          std::vector<CompletionItem> *items) {
  std::string spaces_after_include = " ";
  if (match[3].compare("include") == 0 && match[5].length())
    spaces_after_include = match[4].str();

  std::string prefix =
      match[1].str() + '#' + match[2].str() + "include" + spaces_after_include;
  std::string suffix = match[7].str();

  for (CompletionItem &item : *items) {
    char quote0, quote1;
    if (match[5].compare("<") == 0 ||
        (match[5].length() == 0 && item.use_angle_brackets_))
      quote0 = '<', quote1 = '>';
    else
      quote0 = quote1 = '"';

    item.textEdit.newText =
        prefix + quote0 + item.textEdit.newText + quote1 + suffix;
    item.label = prefix + quote0 + item.label + quote1 + suffix;
    item.filterText = std::nullopt;
  }
}

struct ParseIncludeLineResult {
  bool ok;
  std::string keyword;
  std::string quote;
  std::string pattern;
  std::smatch match;
};

ParseIncludeLineResult ParseIncludeLine(const std::string &line) {
  static const std::regex pattern("(\\s*)"       // [1]: spaces before '#'
                                  "#"            //
                                  "(\\s*)"       // [2]: spaces after '#'
                                  "([^\\s\"<]*)" // [3]: "include"
                                  "(\\s*)"       // [4]: spaces before quote
                                  "([\"<])?"     // [5]: the first quote char
                                  "([^\\s\">]*)" // [6]: path of file
                                  "[\">]?"       //
                                  "(.*)");       // [7]: suffix after quote char
  std::smatch match;
  bool ok = std::regex_match(line, match, pattern);
  return {ok, match[3], match[5], match[6], match};
}

// Pre-filters completion responses before sending to vscode. This results in a
// significantly snappier completion experience as vscode is easily overloaded
// when given 1000+ completion items.
void FilterCandidates(CompletionList &result, const std::string &complete_text,
                      lsPosition begin_pos, lsPosition end_pos,
                      const std::string &buffer_line) {
  assert(begin_pos.line == end_pos.line);
  auto &items = result.items;

  // People usually does not want to insert snippets or parenthesis when
  // changing function or type names, e.g. "str.|()" or "std::|<int>".
  bool has_open_paren = false;
  for (int c = end_pos.character; c < buffer_line.size(); ++c) {
    if (buffer_line[c] == '(' || buffer_line[c] == '<')
      has_open_paren = true;
    if (!isspace(buffer_line[c]))
      break;
  }

  auto finalize = [&]() {
    int max_num = g_config->completion.maxNum;
    if (items.size() > max_num) {
      items.resize(max_num);
      result.isIncomplete = true;
    }

    std::string sort(4, ' ');
    for (auto &item : items) {
      item.textEdit.range = lsRange{begin_pos, end_pos};
      if (has_open_paren && item.filterText)
        item.textEdit.newText = item.filterText.value();
      // https://github.com/Microsoft/language-server-protocol/issues/543
      // Order of textEdit and additionalTextEdits is unspecified.
      auto &edits = item.additionalTextEdits;
      if (edits.size() && edits[0].range.end == begin_pos) {
        lsPosition start = edits[0].range.start, end = edits[0].range.end;
        item.textEdit.range.start = start;
        item.textEdit.newText = edits[0].newText + item.textEdit.newText;
        if (start.line == begin_pos.line && item.filterText) {
          item.filterText =
              buffer_line.substr(start.character,
                                 end.character - start.character) +
              item.filterText.value();
        }
        edits.erase(edits.begin());
      }
      for (auto i = sort.size(); i && ++sort[i - 1] == 'A';)
        sort[--i] = ' ';
      item.sortText = sort;
      // Compatibility
      item.insertText = item.textEdit.newText;
    }
  };

  // No complete text; don't run any filtering logic except to trim the items.
  if (!g_config->completion.filterAndSort || complete_text.empty()) {
    finalize();
    return;
  }

  // Make sure all items have |filterText| set, code that follow needs it.
  for (auto &item : items) {
    if (!item.filterText)
      item.filterText = item.label;
  }

  // Fuzzy match and remove awful candidates.
  bool sensitive = g_config->completion.caseSensitivity;
  FuzzyMatcher fuzzy(complete_text, sensitive);
  for (auto &item : items) {
    item.score_ =
        ReverseSubseqMatch(complete_text, *item.filterText, sensitive) >= 0
            ? fuzzy.Match(*item.filterText)
            : FuzzyMatcher::kMinScore;
  }
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const CompletionItem &item) {
                               return item.score_ <= FuzzyMatcher::kMinScore;
                             }),
              items.end());
  std::sort(items.begin(), items.end(),
            [](const CompletionItem &lhs, const CompletionItem &rhs) {
              int t = int(lhs.additionalTextEdits.size() -
                          rhs.additionalTextEdits.size());
              if (t)
                return t < 0;
              if (lhs.score_ != rhs.score_)
                return lhs.score_ > rhs.score_;
              if (lhs.priority_ != rhs.priority_)
                return lhs.priority_ < rhs.priority_;
              if (lhs.filterText->size() != rhs.filterText->size())
                return lhs.filterText->size() < rhs.filterText->size();
              return *lhs.filterText < *rhs.filterText;
            });

  // Trim result.
  finalize();
}

CompletionItemKind GetCompletionKind(CXCursorKind cursor_kind) {
  switch (cursor_kind) {
  case CXCursor_UnexposedDecl:
    return CompletionItemKind::Text;

  case CXCursor_StructDecl:
  case CXCursor_UnionDecl:
    return CompletionItemKind::Struct;
  case CXCursor_ClassDecl:
    return CompletionItemKind::Class;
  case CXCursor_EnumDecl:
    return CompletionItemKind::Enum;
  case CXCursor_FieldDecl:
    return CompletionItemKind::Field;
  case CXCursor_EnumConstantDecl:
    return CompletionItemKind::EnumMember;
  case CXCursor_FunctionDecl:
    return CompletionItemKind::Function;
  case CXCursor_VarDecl:
  case CXCursor_ParmDecl:
    return CompletionItemKind::Variable;
  case CXCursor_ObjCInterfaceDecl:
    return CompletionItemKind::Interface;

  case CXCursor_ObjCInstanceMethodDecl:
  case CXCursor_CXXMethod:
  case CXCursor_ObjCClassMethodDecl:
    return CompletionItemKind::Method;

  case CXCursor_FunctionTemplate:
    return CompletionItemKind::Function;

  case CXCursor_Constructor:
  case CXCursor_Destructor:
  case CXCursor_ConversionFunction:
    return CompletionItemKind::Constructor;

  case CXCursor_ObjCIvarDecl:
    return CompletionItemKind::Variable;

  case CXCursor_ClassTemplate:
  case CXCursor_ClassTemplatePartialSpecialization:
  case CXCursor_UsingDeclaration:
  case CXCursor_TypedefDecl:
  case CXCursor_TypeAliasDecl:
  case CXCursor_TypeAliasTemplateDecl:
  case CXCursor_ObjCCategoryDecl:
  case CXCursor_ObjCProtocolDecl:
  case CXCursor_ObjCImplementationDecl:
  case CXCursor_ObjCCategoryImplDecl:
    return CompletionItemKind::Class;

  case CXCursor_ObjCPropertyDecl:
    return CompletionItemKind::Property;

  case CXCursor_MacroInstantiation:
  case CXCursor_MacroDefinition:
    return CompletionItemKind::Interface;

  case CXCursor_Namespace:
  case CXCursor_NamespaceAlias:
  case CXCursor_NamespaceRef:
    return CompletionItemKind::Module;

  case CXCursor_MemberRef:
  case CXCursor_TypeRef:
  case CXCursor_ObjCSuperClassRef:
  case CXCursor_ObjCProtocolRef:
  case CXCursor_ObjCClassRef:
    return CompletionItemKind::Reference;

  case CXCursor_NotImplemented:
  case CXCursor_OverloadCandidate:
    return CompletionItemKind::Text;

  case CXCursor_TemplateTypeParameter:
  case CXCursor_TemplateTemplateParameter:
    return CompletionItemKind::TypeParameter;

  default:
    LOG_S(WARNING) << "Unhandled completion kind " << cursor_kind;
    return CompletionItemKind::Text;
  }
}

void BuildItem(const CodeCompletionResult &R, const CodeCompletionString &CCS,
               std::vector<CompletionItem> &out) {
  assert(!out.empty());
  auto first = out.size() - 1;
  bool ignore = false;
  std::string result_type;

  for (const auto &Chunk : CCS) {
    CodeCompletionString::ChunkKind Kind = Chunk.Kind;
    std::string text;
    switch (Kind) {
    case CodeCompletionString::CK_TypedText:
      text = Chunk.Text;
      for (auto i = first; i < out.size(); i++)
        if (Kind == CodeCompletionString::CK_TypedText && !out[i].filterText)
          out[i].filterText = text;
      break;
    case CodeCompletionString::CK_Placeholder:
      text = Chunk.Text;
      for (auto i = first; i < out.size(); i++)
        out[i].parameters_.push_back(text);
      break;
    case CodeCompletionString::CK_Informative:
      if (StringRef(Chunk.Text).endswith("::"))
        continue;
      text = Chunk.Text;
      break;
    case CodeCompletionString::CK_ResultType:
      result_type = Chunk.Text;
      continue;
    case CodeCompletionString::CK_CurrentParameter:
      // This should never be present while collecting completion items.
      llvm_unreachable("unexpected CK_CurrentParameter");
      continue;
    case CodeCompletionString::CK_Optional: {
      // Duplicate last element, the recursive call will complete it.
      if (g_config->completion.duplicateOptional) {
        out.push_back(out.back());
        BuildItem(R, *Chunk.Optional, out);
      }
      continue;
    }
    default:
      text = Chunk.Text;
      break;
    }

    for (auto i = first; i < out.size(); ++i) {
      out[i].label += text;
      if (ignore ||
          (!g_config->client.snippetSupport && out[i].parameters_.size()))
        continue;

      if (Kind == CodeCompletionString::CK_Placeholder) {
        if (R.Kind == CodeCompletionResult::RK_Pattern) {
          ignore = true;
          continue;
        }
        out[i].textEdit.newText +=
            "${" + std::to_string(out[i].parameters_.size()) + ":" + text + "}";
        out[i].insertTextFormat = InsertTextFormat::Snippet;
      } else if (Kind != CodeCompletionString::CK_Informative) {
        out[i].textEdit.newText += text;
      }
    }
  }

  if (result_type.size())
    for (auto i = first; i < out.size(); ++i) {
      // ' : ' for variables,
      // ' -> ' (trailing return type-like) for functions
      out[i].label += (out[i].label == out[i].filterText ? " : " : " -> ");
      out[i].label += result_type;
    }
}

class CompletionConsumer : public CodeCompleteConsumer {
  std::shared_ptr<clang::GlobalCodeCompletionAllocator> Alloc;
  CodeCompletionTUInfo CCTUInfo;

public:
  bool from_cache;
  std::vector<CompletionItem> ls_items;

  CompletionConsumer(const CodeCompleteOptions &Opts, bool from_cache)
      : CodeCompleteConsumer(Opts, false),
        Alloc(std::make_shared<clang::GlobalCodeCompletionAllocator>()),
        CCTUInfo(Alloc), from_cache(from_cache) {}

  void ProcessCodeCompleteResults(Sema &S, CodeCompletionContext Context,
                                  CodeCompletionResult *Results,
                                  unsigned NumResults) override {
    if (Context.getKind() == CodeCompletionContext::CCC_Recovery)
      return;
    ls_items.reserve(NumResults);
    for (unsigned i = 0; i != NumResults; i++) {
      auto &R = Results[i];
      if (R.Availability == CXAvailability_NotAccessible ||
          R.Availability == CXAvailability_NotAvailable)
        continue;
      if (R.Declaration) {
        if (R.Declaration->getKind() == Decl::CXXDestructor)
          continue;
        if (auto *RD = dyn_cast<RecordDecl>(R.Declaration))
          if (RD->isInjectedClassName())
            continue;
        auto NK = R.Declaration->getDeclName().getNameKind();
        if (NK == DeclarationName::CXXOperatorName ||
            NK == DeclarationName::CXXLiteralOperatorName)
          continue;
      }
      CodeCompletionString *CCS = R.CreateCodeCompletionString(
          S, Context, getAllocator(), getCodeCompletionTUInfo(),
          includeBriefComments());
      CompletionItem ls_item;
      ls_item.kind = GetCompletionKind(R.CursorKind);
      if (const char *brief = CCS->getBriefComment())
        ls_item.documentation = brief;
      ls_item.detail = CCS->getParentContextName().str();

      size_t first_idx = ls_items.size();
      ls_items.push_back(ls_item);
      BuildItem(R, *CCS, ls_items);

      for (size_t j = first_idx; j < ls_items.size(); j++) {
        if (g_config->client.snippetSupport &&
            ls_items[j].insertTextFormat == InsertTextFormat::Snippet)
          ls_items[j].textEdit.newText += "$0";
        ls_items[j].priority_ = CCS->getPriority();
        if (!g_config->completion.detailedLabel) {
          ls_items[j].detail = ls_items[j].label;
          ls_items[j].label = ls_items[j].filterText.value_or("");
        }
      }
#if LLVM_VERSION_MAJOR >= 7
      for (const FixItHint &FixIt : R.FixIts) {
        auto &AST = S.getASTContext();
        TextEdit ls_edit =
            ccls::ToTextEdit(AST.getSourceManager(), AST.getLangOpts(), FixIt);
        for (size_t j = first_idx; j < ls_items.size(); j++)
          ls_items[j].additionalTextEdits.push_back(ls_edit);
      }
#endif
    }
  }

  CodeCompletionAllocator &getAllocator() override { return *Alloc; }
  CodeCompletionTUInfo &getCodeCompletionTUInfo() override { return CCTUInfo; }
};
} // namespace

void MessageHandler::textDocument_completion(CompletionParam &param,
                                             ReplyOnce &reply) {
  static CompleteConsumerCache<std::vector<CompletionItem>> cache;
  CompletionList result;
  std::string path = param.textDocument.uri.GetPath();
  WorkingFile *file = wfiles->GetFileByFilename(path);
  if (!file) {
    return;
  }

  // It shouldn't be possible, but sometimes vscode will send queries out
  // of order, ie, we get completion request before buffer content update.
  std::string buffer_line;
  if (param.position.line >= 0 &&
      param.position.line < file->buffer_lines.size())
    buffer_line = file->buffer_lines[param.position.line];

  // Check for - and : before completing -> or ::, since vscode does not
  // support multi-character trigger characters.
  if (param.context.triggerKind == CompletionTriggerKind::TriggerCharacter &&
      param.context.triggerCharacter) {
    bool did_fail_check = false;

    std::string character = *param.context.triggerCharacter;
    int preceding_index = param.position.character - 2;

    // If the character is '"', '<' or '/', make sure that the line starts
    // with '#'.
    if (character == "\"" || character == "<" || character == "/") {
      size_t i = 0;
      while (i < buffer_line.size() && isspace(buffer_line[i]))
        ++i;
      if (i >= buffer_line.size() || buffer_line[i] != '#')
        did_fail_check = true;
    }
    // If the character is > or : and we are at the start of the line, do not
    // show completion results.
    else if ((character == ">" || character == ":") && preceding_index < 0) {
      did_fail_check = true;
    }
    // If the character is > but - does not preced it, or if it is : and :
    // does not preced it, do not show completion results.
    else if (preceding_index >= 0 &&
             preceding_index < (int)buffer_line.size()) {
      char preceding = buffer_line[preceding_index];
      did_fail_check = (preceding != '-' && character == ">") ||
                       (preceding != ':' && character == ":");
    }

    if (did_fail_check) {
      reply(result);
      return;
    }
  }

  std::string completion_text;
  lsPosition end_pos = param.position;
  lsPosition begin_pos = file->FindStableCompletionSource(
      param.position, &completion_text, &end_pos);

  ParseIncludeLineResult preprocess = ParseIncludeLine(buffer_line);

  if (preprocess.ok && preprocess.keyword.compare("include") == 0) {
    CompletionList result;
    {
      std::unique_lock<std::mutex> lock(
          include_complete->completion_items_mutex, std::defer_lock);
      if (include_complete->is_scanning)
        lock.lock();
      std::string quote = preprocess.match[5];
      for (auto &item : include_complete->completion_items)
        if (quote.empty() || quote == (item.use_angle_brackets_ ? "<" : "\""))
          result.items.push_back(item);
    }
    begin_pos.character = 0;
    end_pos.character = (int)buffer_line.size();
    FilterCandidates(result, preprocess.pattern, begin_pos, end_pos,
                     buffer_line);
    DecorateIncludePaths(preprocess.match, &result.items);
    reply(result);
  } else {
    std::string path = param.textDocument.uri.GetPath();
    CompletionManager::OnComplete callback =
        [completion_text, path, begin_pos, end_pos, reply,
         buffer_line](CodeCompleteConsumer *OptConsumer) {
          if (!OptConsumer)
            return;
          auto *Consumer = static_cast<CompletionConsumer *>(OptConsumer);
          CompletionList result;
          result.items = Consumer->ls_items;

          FilterCandidates(result, completion_text, begin_pos, end_pos,
                           buffer_line);
          reply(result);
          if (!Consumer->from_cache) {
            cache.WithLock([&]() {
              cache.path = path;
              cache.position = begin_pos;
              cache.result = Consumer->ls_items;
            });
          }
        };

    clang::CodeCompleteOptions CCOpts;
    CCOpts.IncludeBriefComments = true;
    CCOpts.IncludeCodePatterns = preprocess.ok; // if there is a #
#if LLVM_VERSION_MAJOR >= 7
    CCOpts.IncludeFixIts = true;
#endif
    CCOpts.IncludeMacros = true;
    if (cache.IsCacheValid(path, begin_pos)) {
      CompletionConsumer Consumer(CCOpts, true);
      cache.WithLock([&]() { Consumer.ls_items = cache.result; });
      callback(&Consumer);
    } else {
      clang_complete->completion_request_.PushBack(
          std::make_unique<CompletionManager::CompletionRequest>(
              reply.id, param.textDocument, begin_pos,
              std::make_unique<CompletionConsumer>(CCOpts, false), CCOpts,
              callback));
    }
  }
}
} // namespace ccls
