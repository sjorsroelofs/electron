// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <string>

#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_converters/gurl_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/error_thrower.h"
#include "shell/common/node_includes.h"
#include "shell/common/platform_util.h"
#include "shell/common/promise_util.h"

#if defined(OS_WIN)
#include "base/win/scoped_com_initializer.h"
#include "base/win/shortcut.h"

namespace gin {

template <>
struct Converter<base::win::ShortcutOperation> {
  static bool FromV8(v8::Isolate* isolate,
                     v8::Handle<v8::Value> val,
                     base::win::ShortcutOperation* out) {
    std::string operation;
    if (!ConvertFromV8(isolate, val, &operation))
      return false;
    if (operation.empty() || operation == "create")
      *out = base::win::SHORTCUT_CREATE_ALWAYS;
    else if (operation == "update")
      *out = base::win::SHORTCUT_UPDATE_EXISTING;
    else if (operation == "replace")
      *out = base::win::SHORTCUT_REPLACE_EXISTING;
    else
      return false;
    return true;
  }
};

}  // namespace gin
#endif

namespace {

void OnOpenExternalFinished(electron::util::Promise<void*> promise,
                            const std::string& error) {
  if (error.empty())
    promise.Resolve();
  else
    promise.RejectWithErrorMessage(error.c_str());
}

v8::Local<v8::Promise> OpenExternal(const GURL& url, gin::Arguments* args) {
  electron::util::Promise<void*> promise(args->isolate());
  v8::Local<v8::Promise> handle = promise.GetHandle();

  platform_util::OpenExternalOptions options;
  if (args->Length() >= 2) {
    gin::Dictionary obj(nullptr);
    if (args->GetNext(&obj)) {
      obj.Get("activate", &options.activate);
      obj.Get("workingDirectory", &options.working_dir);
    }
  }

  platform_util::OpenExternal(
      url, options,
      base::BindOnce(&OnOpenExternalFinished, std::move(promise)));
  return handle;
}

bool MoveItemToTrash(gin::Arguments* args) {
  base::FilePath full_path;
  args->GetNext(&full_path);

  bool delete_on_fail = false;
  args->GetNext(&delete_on_fail);

  return platform_util::MoveItemToTrash(full_path, delete_on_fail);
}

#if defined(OS_WIN)
bool WriteShortcutLink(const base::FilePath& shortcut_path,
                       gin_helper::Arguments* args) {
  base::win::ShortcutOperation operation = base::win::SHORTCUT_CREATE_ALWAYS;
  args->GetNext(&operation);
  gin::Dictionary options = gin::Dictionary::CreateEmpty(args->isolate());
  if (!args->GetNext(&options)) {
    args->ThrowError();
    return false;
  }

  base::win::ShortcutProperties properties;
  base::FilePath path;
  base::string16 str;
  int index;
  if (options.Get("target", &path))
    properties.set_target(path);
  if (options.Get("cwd", &path))
    properties.set_working_dir(path);
  if (options.Get("args", &str))
    properties.set_arguments(str);
  if (options.Get("description", &str))
    properties.set_description(str);
  if (options.Get("icon", &path) && options.Get("iconIndex", &index))
    properties.set_icon(path, index);
  if (options.Get("appUserModelId", &str))
    properties.set_app_id(str);

  base::win::ScopedCOMInitializer com_initializer;
  return base::win::CreateOrUpdateShortcutLink(shortcut_path, properties,
                                               operation);
}

v8::Local<v8::Value> ReadShortcutLink(gin_helper::ErrorThrower thrower,
                                      const base::FilePath& path) {
  using base::win::ShortcutProperties;
  gin::Dictionary options = gin::Dictionary::CreateEmpty(thrower.isolate());
  base::win::ScopedCOMInitializer com_initializer;
  base::win::ShortcutProperties properties;
  if (!base::win::ResolveShortcutProperties(
          path, ShortcutProperties::PROPERTIES_ALL, &properties)) {
    thrower.ThrowError("Failed to read shortcut link");
    return v8::Null(thrower.isolate());
  }
  options.Set("target", properties.target);
  options.Set("cwd", properties.working_dir);
  options.Set("args", properties.arguments);
  options.Set("description", properties.description);
  options.Set("icon", properties.icon);
  options.Set("iconIndex", properties.icon_index);
  options.Set("appUserModelId", properties.app_id);
  return gin::ConvertToV8(thrower.isolate(), options);
}
#endif

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  gin_helper::Dictionary dict(context->GetIsolate(), exports);
  dict.SetMethod("showItemInFolder", &platform_util::ShowItemInFolder);
  dict.SetMethod("openItem", &platform_util::OpenItem);
  dict.SetMethod("openExternal", &OpenExternal);
  dict.SetMethod("moveItemToTrash", &MoveItemToTrash);
  dict.SetMethod("beep", &platform_util::Beep);
#if defined(OS_WIN)
  dict.SetMethod("writeShortcutLink", &WriteShortcutLink);
  dict.SetMethod("readShortcutLink", &ReadShortcutLink);
#endif
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(atom_common_shell, Initialize)
