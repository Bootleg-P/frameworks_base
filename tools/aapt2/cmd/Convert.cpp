/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>

#include "android-base/macros.h"
#include "androidfw/StringPiece.h"

#include "Flags.h"
#include "LoadedApk.h"
#include "ValueVisitor.h"
#include "cmd/Util.h"
#include "format/binary/TableFlattener.h"
#include "format/binary/XmlFlattener.h"
#include "format/proto/ProtoDeserialize.h"
#include "format/proto/ProtoSerialize.h"
#include "io/BigBufferStream.h"
#include "io/Util.h"
#include "process/IResourceTableConsumer.h"
#include "process/SymbolTable.h"
#include "util/Util.h"

using ::android::StringPiece;
using ::android::base::StringPrintf;
using ::std::unique_ptr;
using ::std::vector;

namespace aapt {

class IApkSerializer {
 public:
  IApkSerializer(IAaptContext* context, const Source& source) : context_(context), source_(source) {}

  virtual bool SerializeXml(const xml::XmlResource* xml, const std::string& path, bool utf16,
                            IArchiveWriter* writer) = 0;
  virtual bool SerializeTable(ResourceTable* table, IArchiveWriter* writer) = 0;
  virtual bool SerializeFile(FileReference* file, IArchiveWriter* writer) = 0;

  virtual ~IApkSerializer() = default;

 protected:
  IAaptContext* context_;
  Source source_;
};

bool ConvertApk(IAaptContext* context, unique_ptr<LoadedApk> apk, IApkSerializer* serializer,
                IArchiveWriter* writer) {
  if (!serializer->SerializeXml(apk->GetManifest(), kAndroidManifestPath, true /*utf16*/, writer)) {
    context->GetDiagnostics()->Error(DiagMessage(apk->GetSource())
                                     << "failed to serialize AndroidManifest.xml");
    return false;
  }

  if (apk->GetResourceTable() != nullptr) {
    // The table might be modified by below code.
    auto converted_table = apk->GetResourceTable();

    // Resources
    for (const auto& package : converted_table->packages) {
      for (const auto& type : package->types) {
        for (const auto& entry : type->entries) {
          for (const auto& config_value : entry->values) {
            FileReference* file = ValueCast<FileReference>(config_value->value.get());
            if (file != nullptr) {
              if (file->file == nullptr) {
                context->GetDiagnostics()->Error(DiagMessage(apk->GetSource())
                                                 << "no file associated with " << *file);
                return false;
              }

              if (!serializer->SerializeFile(file, writer)) {
                context->GetDiagnostics()->Error(DiagMessage(apk->GetSource())
                                                 << "failed to serialize file " << *file->path);
                return false;
              }
            } // file
          } // config_value
        } // entry
      } // type
    } // package

    // Converted resource table
    if (!serializer->SerializeTable(converted_table, writer)) {
      context->GetDiagnostics()->Error(DiagMessage(apk->GetSource())
                                       << "failed to serialize the resource table");
      return false;
    }
  }

  // Other files
  std::unique_ptr<io::IFileCollectionIterator> iterator = apk->GetFileCollection()->Iterator();
  while (iterator->HasNext()) {
    io::IFile* file = iterator->Next();

    std::string path = file->GetSource().path;
    // The name of the path has the format "<zip-file-name>@<path-to-file>".
    path = path.substr(path.find('@') + 1);

    // Manifest, resource table and resources have already been taken care of.
    if (path == kAndroidManifestPath ||
        path == kApkResourceTablePath ||
        path == kProtoResourceTablePath ||
        path.find("res/") == 0) {
      continue;
    }

    if (!io::CopyFileToArchivePreserveCompression(context, file, path, writer)) {
      context->GetDiagnostics()->Error(DiagMessage(apk->GetSource())
                                       << "failed to copy file " << path);
      return false;
    }
  }

  return true;
}


class BinaryApkSerializer : public IApkSerializer {
 public:
  BinaryApkSerializer(IAaptContext* context, const Source& source,
                   const TableFlattenerOptions& options)
      : IApkSerializer(context, source), tableFlattenerOptions_(options) {}

  bool SerializeXml(const xml::XmlResource* xml, const std::string& path, bool utf16,
                    IArchiveWriter* writer) override {
    BigBuffer buffer(4096);
    XmlFlattenerOptions options = {};
    options.use_utf16 = utf16;
    options.keep_raw_values = true;
    XmlFlattener flattener(&buffer, options);
    if (!flattener.Consume(context_, xml)) {
      return false;
    }

    io::BigBufferInputStream input_stream(&buffer);
    return io::CopyInputStreamToArchive(context_, &input_stream, path, 0u,
                                        writer);
  }

  bool SerializeTable(ResourceTable* table, IArchiveWriter* writer) override {
    BigBuffer buffer(4096);
    TableFlattener table_flattener(tableFlattenerOptions_, &buffer);
    if (!table_flattener.Consume(context_, table)) {
      return false;
    }

    io::BigBufferInputStream input_stream(&buffer);
    return io::CopyInputStreamToArchive(context_, &input_stream, kApkResourceTablePath,
                                        ArchiveEntry::kAlign, writer);
  }

  bool SerializeFile(FileReference* file, IArchiveWriter* writer) override {
    if (file->type == ResourceFile::Type::kProtoXml) {
      unique_ptr<io::InputStream> in = file->file->OpenInputStream();
      if (in == nullptr) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to open file " << *file->path);
        return false;
      }

      pb::XmlNode pb_node;
      io::ZeroCopyInputAdaptor adaptor(in.get());
      if (!pb_node.ParseFromZeroCopyStream(&adaptor)) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to parse proto XML " << *file->path);
        return false;
      }

      std::string error;
      unique_ptr<xml::XmlResource> xml = DeserializeXmlResourceFromPb(pb_node, &error);
      if (xml == nullptr) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to deserialize proto XML "
                                          << *file->path << ": " << error);
        return false;
      }

      if (!SerializeXml(xml.get(), *file->path, false /*utf16*/, writer)) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to serialize to binary XML: " << *file->path);
        return false;
      }

      file->type = ResourceFile::Type::kBinaryXml;
    } else {
      if (!io::CopyFileToArchivePreserveCompression(context_, file->file, *file->path, writer)) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to copy file " << *file->path);
        return false;
      }
    }

    return true;
  }

 private:
  TableFlattenerOptions tableFlattenerOptions_;

  DISALLOW_COPY_AND_ASSIGN(BinaryApkSerializer);
};

class ProtoApkSerializer : public IApkSerializer {
 public:
  ProtoApkSerializer(IAaptContext* context, const Source& source)
      : IApkSerializer(context, source) {}

  bool SerializeXml(const xml::XmlResource* xml, const std::string& path, bool utf16,
                    IArchiveWriter* writer) override {
    pb::XmlNode pb_node;
    SerializeXmlResourceToPb(*xml, &pb_node);
    return io::CopyProtoToArchive(context_, &pb_node, path, 0u, writer);
  }

  bool SerializeTable(ResourceTable* table, IArchiveWriter* writer) override {
    pb::ResourceTable pb_table;
    SerializeTableToPb(*table, &pb_table, context_->GetDiagnostics());
    return io::CopyProtoToArchive(context_, &pb_table, kProtoResourceTablePath,
                                  0u, writer);
  }

  bool SerializeFile(FileReference* file, IArchiveWriter* writer) override {
    if (file->type == ResourceFile::Type::kBinaryXml) {
      std::unique_ptr<io::IData> data = file->file->OpenAsData();
      if (!data) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                         << "failed to open file " << *file->path);
        return false;
      }

      std::string error;
      std::unique_ptr<xml::XmlResource> xml = xml::Inflate(data->data(), data->size(), &error);
      if (xml == nullptr) {
        context_->GetDiagnostics()->Error(DiagMessage(source_) << "failed to parse binary XML: "
                                          << error);
        return false;
      }

      if (!SerializeXml(xml.get(), *file->path, false /*utf16*/, writer)) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to serialize to proto XML: " << *file->path);
        return false;
      }

      file->type = ResourceFile::Type::kProtoXml;
    } else {
      if (!io::CopyFileToArchivePreserveCompression(context_, file->file, *file->path, writer)) {
        context_->GetDiagnostics()->Error(DiagMessage(source_)
                                          << "failed to copy file " << *file->path);
        return false;
      }
    }

    return true;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ProtoApkSerializer);
};

class Context : public IAaptContext {
 public:
  Context() : mangler_({}), symbols_(&mangler_) {
  }

  PackageType GetPackageType() override {
    return PackageType::kApp;
  }

  SymbolTable* GetExternalSymbols() override {
    return &symbols_;
  }

  IDiagnostics* GetDiagnostics() override {
    return &diag_;
  }

  const std::string& GetCompilationPackage() override {
    return package_;
  }

  uint8_t GetPackageId() override {
    // Nothing should call this.
    UNIMPLEMENTED(FATAL) << "PackageID should not be necessary";
    return 0;
  }

  NameMangler* GetNameMangler() override {
    UNIMPLEMENTED(FATAL);
    return nullptr;
  }

  bool IsVerbose() override {
    return verbose_;
  }

  int GetMinSdkVersion() override {
    return 0u;
  }

  bool verbose_ = false;
  std::string package_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Context);

  NameMangler mangler_;
  SymbolTable symbols_;
  StdErrDiagnostics diag_;
};

int Convert(const vector<StringPiece>& args) {

  static const char* kOutputFormatProto = "proto";
  static const char* kOutputFormatBinary = "binary";

  Context context;
  std::string output_path;
  Maybe<std::string> output_format;
  TableFlattenerOptions options;
  Flags flags =
      Flags()
          .RequiredFlag("-o", "Output path", &output_path)
          .OptionalFlag("--output-format", StringPrintf("Format of the output. Accepted values are "
                        "'%s' and '%s'. When not set, defaults to '%s'.", kOutputFormatProto,
                        kOutputFormatBinary, kOutputFormatBinary), &output_format)
          .OptionalSwitch("--enable-sparse-encoding",
                          "Enables encoding sparse entries using a binary search tree.\n"
                          "This decreases APK size at the cost of resource retrieval performance.",
                          &options.use_sparse_entries)
          .OptionalSwitch("-v", "Enables verbose logging", &context.verbose_);
  if (!flags.Parse("aapt2 convert", args, &std::cerr)) {
    return 1;
  }

  if (flags.GetArgs().size() != 1) {
    std::cerr << "must supply a single proto APK\n";
    flags.Usage("aapt2 convert", &std::cerr);
    return 1;
  }

  const StringPiece& path = flags.GetArgs()[0];
  unique_ptr<LoadedApk> apk = LoadedApk::LoadApkFromPath(path, context.GetDiagnostics());
  if (apk == nullptr) {
    context.GetDiagnostics()->Error(DiagMessage(path) << "failed to load APK");
    return 1;
  }

  Maybe<AppInfo> app_info =
      ExtractAppInfoFromBinaryManifest(*apk->GetManifest(), context.GetDiagnostics());
  if (!app_info) {
    return 1;
  }

  context.package_ = app_info.value().package;

  unique_ptr<IArchiveWriter> writer =
      CreateZipFileArchiveWriter(context.GetDiagnostics(), output_path);
  if (writer == nullptr) {
    return 1;
  }

  unique_ptr<IApkSerializer> serializer;
  if (!output_format || output_format.value() == kOutputFormatBinary) {
    serializer.reset(new BinaryApkSerializer(&context, apk->GetSource(), options));
  } else if (output_format.value() == kOutputFormatProto) {
    serializer.reset(new ProtoApkSerializer(&context, apk->GetSource()));
  } else {
    context.GetDiagnostics()->Error(DiagMessage(path)
                                    << "Invalid value for flag --output-format: "
                                    << output_format.value());
    return 1;
  }


  return ConvertApk(&context, std::move(apk), serializer.get(), writer.get()) ? 0 : 1;
}

}  // namespace aapt
