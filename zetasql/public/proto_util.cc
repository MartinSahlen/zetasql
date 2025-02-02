//
// Copyright 2019 ZetaSQL Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/public/proto_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetasql/base/logging.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/wire_format_lite.h"
#include "zetasql/public/civil_time.h"
#include "zetasql/public/functions/arithmetics.h"
#include "zetasql/public/functions/date_time_util.h"
#include "zetasql/public/numeric_value.h"
#include "zetasql/public/proto/type_annotation.pb.h"
#include "zetasql/public/type.h"
#include "zetasql/public/type.pb.h"
#include "zetasql/public/value.h"
#include "absl/base/casts.h"
#include <cstdint>
#include "absl/base/optimization.h"
#include "absl/container/inlined_vector.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_cat.h"
#include "absl/types/variant.h"
#include "zetasql/base/map_util.h"
#include "zetasql/base/source_location.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"

// This flag is only for testing the non-optimized path when reading one proto
// field.
ABSL_FLAG(bool, zetasql_read_proto_field_optimized_path, true,
          "Use the specialized version of ReadProtoFields which looks for "
          "only one field.");

using google::protobuf::internal::WireFormatLite;

namespace zetasql {

#define RETURN_ERROR_IF_INVALID_DEFAULT_VALUE(valid, field)                 \
  while (ABSL_PREDICT_FALSE(!(valid)))                                      \
  return ::zetasql_base::InvalidArgumentErrorBuilder(ZETASQL_LOC)                       \
         << "Unable to decode default value for " << (field)->DebugString() \
         << "\n(value out of valid range)"

static zetasql_base::Status GetProtoFieldDefaultImpl(
    const google::protobuf::FieldDescriptor* field, const Type* type,
    bool ignore_use_defaults_annotations, bool ignore_format_annotations,
    Value* default_value) {
  if (ZETASQL_DEBUG_MODE) {
    TypeKind field_kind;
    ZETASQL_RET_CHECK(ProtoType::FieldDescriptorToTypeKind(ignore_format_annotations,
                                                   field, &field_kind)
                  .ok());
    ZETASQL_RET_CHECK_EQ(type->kind(), field_kind);
  }

  if (field->is_required()) {
    // There is no default for a required field so don't return a valid value.
    *default_value = Value();
    return ::zetasql_base::OkStatus();
  }

  if (type->IsArray()) {
    // Missing repeated fields are treated as empty arrays.
    *default_value = Value::EmptyArray(type->AsArray());
    return ::zetasql_base::OkStatus();
  }

  if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE ||
      field->type() == google::protobuf::FieldDescriptor::TYPE_GROUP) {
    *default_value = Value::Null(type);
    return ::zetasql_base::OkStatus();
  }

  const bool use_defaults = ProtoType::GetUseDefaultsExtension(field);
  if (!use_defaults && !ignore_use_defaults_annotations) {
    *default_value = Value::Null(type);
    return ::zetasql_base::OkStatus();
  }

  int64_t datetime_value;
  if (type->kind() == TYPE_DATE || type->kind() == TYPE_TIMESTAMP ||
      type->kind() == TYPE_TIME || type->kind() == TYPE_DATETIME) {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_INT32:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
      case google::protobuf::FieldDescriptor::TYPE_SINT32:
        datetime_value = field->default_value_int32();
        break;
      case google::protobuf::FieldDescriptor::TYPE_INT64:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
      case google::protobuf::FieldDescriptor::TYPE_SINT64:
        datetime_value = field->default_value_int64();
        break;
      case google::protobuf::FieldDescriptor::TYPE_UINT64:
        if ( type->kind() == TYPE_TIMESTAMP) {
          datetime_value = absl::bit_cast<int64_t>(field->default_value_uint64());
          break;
        }
        ABSL_FALLTHROUGH_INTENDED;
      default:
        return ::zetasql_base::InvalidArgumentErrorBuilder()
               << "Invalid date/time annotation on " << field->DebugString();
    }
  }
  switch (type->kind()) {
    case TYPE_INT32:
      *default_value = Value::Int32(field->default_value_int32());
      break;
    case TYPE_INT64:
      *default_value = Value::Int64(field->default_value_int64());
      break;
    case TYPE_UINT32:
      *default_value = Value::Uint32(field->default_value_uint32());
      break;
    case TYPE_UINT64:
      *default_value = Value::Uint64(field->default_value_uint64());
      break;
    case TYPE_BOOL:
      *default_value = Value::Bool(field->default_value_bool());
      break;
    case TYPE_FLOAT:
      *default_value = Value::Float(field->default_value_float());
      break;
    case TYPE_DOUBLE:
      *default_value = Value::Double(field->default_value_double());
      break;
    case TYPE_STRING:
      *default_value = Value::String(field->default_value_string());
      break;
    case TYPE_BYTES:
      *default_value = Value::Bytes(field->default_value_string());
      break;
    case TYPE_ENUM:
      *default_value = Value::Enum(
          type->AsEnum(), field->default_value_enum()->number());
      break;
    case TYPE_NUMERIC:
      *default_value = Value::Numeric(NumericValue());
      break;
    case TYPE_DATE: {
      const FieldFormat::Format format = ProtoType::GetFormatAnnotation(field);

      int32_t decoded_date;
      bool is_null;
      zetasql_base::Status status = functions::DecodeFormattedDate(
          datetime_value, format, &decoded_date, &is_null);
      if (!status.ok()) {
        return ::zetasql_base::StatusBuilder(status.code(), ZETASQL_LOC)
               << "Unable to decode default value for " << field->DebugString()
               << ": " << status.message();
      }
      if (is_null) {
        *default_value = Value::NullDate();
      } else {
        RETURN_ERROR_IF_INVALID_DEFAULT_VALUE(
            functions::IsValidDate(decoded_date), field);
        *default_value = Value::Date(decoded_date);
      }
      break;
    }
    case TYPE_TIMESTAMP: {
      // For the new timestamp type, widen or truncate the value given the
      // field's annotation.
      const FieldFormat::Format format = ProtoType::GetFormatAnnotation(field);
      switch (format) {
        case FieldFormat::TIMESTAMP_SECONDS:
          RETURN_ERROR_IF_INVALID_DEFAULT_VALUE(
              (functions::IsValidTimestamp(datetime_value,
                                           functions::kSeconds)),
              field);
          datetime_value *= 1000000;
          break;
        case FieldFormat::TIMESTAMP_MILLIS:
          RETURN_ERROR_IF_INVALID_DEFAULT_VALUE(
              functions::IsValidTimestamp(datetime_value,
                                          functions::kMilliseconds),
              field);
          datetime_value *= 1000;
          break;
        case FieldFormat::TIMESTAMP_MICROS:
          RETURN_ERROR_IF_INVALID_DEFAULT_VALUE(
              functions::IsValidTimestamp(datetime_value,
                                          functions::kMicroseconds),
              field);
          break;
        case FieldFormat::TIMESTAMP_NANOS:
          RETURN_ERROR_IF_INVALID_DEFAULT_VALUE(
              functions::IsValidTimestamp(datetime_value,
                                          functions::kNanoseconds),
              field);
          datetime_value /= 1000;
          break;
        default:
          return ::zetasql_base::InvalidArgumentErrorBuilder()
                 << "Invalid field format for " << field->DebugString();
      }
      *default_value = Value::TimestampFromUnixMicros(datetime_value);
      break;
    }
    case TYPE_TIME: {
      TimeValue time = TimeValue::FromPacked64Micros(datetime_value);
      if (!time.IsValid()) {
        return ::zetasql_base::InvalidArgumentErrorBuilder()
               << "Unable to decode default value for " << field->DebugString();
      }
      *default_value = Value::Time(time);
      break;
    }
    case TYPE_DATETIME: {
      if (datetime_value == 0) {
        *default_value = Value::NullDatetime();
        break;
      }
      DatetimeValue datetime =
          DatetimeValue::FromPacked64Micros(datetime_value);
      if (!datetime.IsValid()) {
        return ::zetasql_base::InvalidArgumentErrorBuilder()
               << "Unable to decode default value for " << field->DebugString();
      }
      *default_value = Value::Datetime(datetime);
      break;
    }
    default:
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "No default value for " << field->DebugString();
  }
  return ::zetasql_base::OkStatus();
}

zetasql_base::Status GetProtoFieldDefaultV2(const google::protobuf::FieldDescriptor* field,
                                    const Type* type, Value* default_value) {
  const bool& ignore_use_defaults =
      field->containing_type()->file()->syntax() ==
      google::protobuf::FileDescriptor::SYNTAX_PROTO3;
  return GetProtoFieldDefaultImpl(field, type, ignore_use_defaults,
                                  /*ignore_format_annotations=*/false,
                                  default_value);
}

zetasql_base::Status GetProtoFieldDefault(const google::protobuf::FieldDescriptor* field,
                                  const Type* type, Value* default_value) {
  return GetProtoFieldDefault(field, type, /*use_obsolete_timestamp=*/false,
                              default_value);
}

zetasql_base::Status GetProtoFieldDefault(const google::protobuf::FieldDescriptor* field,
                                  const Type* type, bool use_obsolete_timestamp,
                                  Value* default_value) {
  ZETASQL_RET_CHECK(!use_obsolete_timestamp);
  return GetProtoFieldDefaultImpl(field, type,
                                  /*ignore_use_defaults_annotations=*/false,
                                  /*ignore_format_annotations=*/false,
                                  default_value);
}

zetasql_base::Status GetProtoFieldDefaultRaw(const google::protobuf::FieldDescriptor* field,
                                     const Type* type, Value* default_value) {
  if (type->IsSimpleType() || type->IsArray()) {
    return GetProtoFieldDefaultImpl(field, type,
                                    /*ignore_use_defaults_annotations=*/true,
                                    /*ignore_format_annotations=*/true,
                                    default_value);
  }
  *default_value = values::Null(type);
  return zetasql_base::OkStatus();
}

static zetasql_base::Status GetProtoFieldTypeAndDefaultImpl(
    const google::protobuf::FieldDescriptor* field, bool ignore_annotations,
    TypeFactory* type_factory, const Type** type, Value* default_value) {
    ZETASQL_RETURN_IF_ERROR(
        type_factory->GetProtoFieldType(ignore_annotations, field, type));
    if (default_value != nullptr) {
      if (ignore_annotations) {
        ZETASQL_RETURN_IF_ERROR(GetProtoFieldDefaultRaw(field, *type, default_value));
      } else {
        ZETASQL_RETURN_IF_ERROR(GetProtoFieldDefault(field, *type, default_value));
      }
    }

  DCHECK(default_value == nullptr ||
         !default_value->is_valid() ||
         default_value->type_kind() == (*type)->kind());

  if (ZETASQL_DEBUG_MODE) {
    // For testing, make sure the TypeKinds we get from
    // FieldDescriptorToTypeKind match the Types returned by this method.
    TypeKind computed_type_kind;
    ZETASQL_RETURN_IF_ERROR(ProtoType::FieldDescriptorToTypeKind(
        ignore_annotations, field, &computed_type_kind));
    ZETASQL_RET_CHECK_EQ((*type)->kind(), computed_type_kind)
        << (*type)->DebugString() << "\n" << field->DebugString();
  }

  return ::zetasql_base::OkStatus();
}

zetasql_base::Status GetProtoFieldTypeAndDefault(const google::protobuf::FieldDescriptor* field,
                                         TypeFactory* type_factory,
                                         const Type** type,
                                         Value* default_value) {
  return GetProtoFieldTypeAndDefault(field, type_factory,
                                     /*use_obsolete_timestamp=*/false, type,
                                     default_value);
}

zetasql_base::Status GetProtoFieldTypeAndDefault(const google::protobuf::FieldDescriptor* field,
                                         TypeFactory* type_factory,
                                         bool use_obsolete_timestamp,
                                         const Type** type,
                                         Value* default_value) {
  ZETASQL_RET_CHECK(!use_obsolete_timestamp);
  return GetProtoFieldTypeAndDefaultImpl(field, /*ignore_annotations=*/false,
                                         type_factory, type, default_value);
}

zetasql_base::Status GetProtoFieldTypeAndDefaultRaw(
    const google::protobuf::FieldDescriptor* field, TypeFactory* type_factory,
    const Type** type, Value* default_value) {
  return GetProtoFieldTypeAndDefaultImpl(field, /*ignore_annotations=*/true,
                                         type_factory, type, default_value);
}

static zetasql_base::Status Int64ToAdjustedTimestampInt64(FieldFormat::Format format,
                                                  int64_t s, int64_t* adjusted_s) {
  zetasql_base::Status status;
  switch (format) {
    case FieldFormat::TIMESTAMP_SECONDS:
      if (!functions::Multiply<int64_t>(s, 1000000LL, adjusted_s, &status)) {
        return status;
      }
      break;
    case FieldFormat::TIMESTAMP_MILLIS:
      if (!functions::Multiply<int64_t>(s, 1000LL, adjusted_s, &status)) {
        return status;
      }
      break;
    case FieldFormat::TIMESTAMP_MICROS:
      *adjusted_s = s;
      break;
    default:
      return ::zetasql_base::OutOfRangeErrorBuilder()
             << "Invalid timestamp field format: " << format;
  }
  return ::zetasql_base::OkStatus();
}

namespace {

// Convenient representation of the intermediate values produced by
// ReadWireValue.
using WireValueType = absl::variant<
    // TYPE_INT32
    // TYPE_SINT32
    // TYPE_SFIXED32
    // TYPE_ENUM
    int32_t,
    // TYPE_INT64
    // TYPE_SINT64
    // TYPE_SFIXED64
    int64_t,
    // TYPE_UINT32
    // TYPE_FIXED32
    uint32_t,
    // TYPE_UINT64
    // TYPE_FIXED64
    uint64_t,
    // TYPE_BOOL
    bool,
    // TYPE_FLOAT
    float,
    // TYPE_DOUBLE
    double,
    // TYPE_MESSAGE
    // TYPE_GROUP
    // TYPE_STRING
    // TYPE_BYTES
    std::string>;

}  // namespace

// Populates 'value' with the value of the field at the front of 'in' (which is
// backed by 'bytes'), without considering any zetasql semantics.
static bool ReadWireValue(
    google::protobuf::FieldDescriptor::Type field_type,
    uint32_t tag_and_type,
    const std::string& bytes,
    google::protobuf::io::CodedInputStream* in,
    WireValueType* value) {
  int32_t i32;
  int64_t i64;
  uint32_t ui32;
  uint64_t ui64;
  bool b;
  float f;
  double d;

  const WireFormatLite::WireType wire_type =
      WireFormatLite::GetTagWireType(tag_and_type);
  switch (field_type) {
    case WireFormatLite::TYPE_INT32:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<int32_t, WireFormatLite::TYPE_INT32>(
              in, &i32)) {
        *value = i32;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_INT64:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<int64_t, WireFormatLite::TYPE_INT64>(
              in, &i64)) {
        *value = i64;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_UINT32:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<uint32_t, WireFormatLite::TYPE_UINT32>(
              in, &ui32)) {
        *value = ui32;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_UINT64:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<uint64_t, WireFormatLite::TYPE_UINT64>(
              in, &ui64)) {
        *value = ui64;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_SINT32:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<int32_t, WireFormatLite::TYPE_SINT32>(
              in, &i32)) {
        *value = i32;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_SINT64:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<int64_t, WireFormatLite::TYPE_SINT64>(
              in, &i64)) {
        *value = i64;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_FIXED32:
      if (wire_type == WireFormatLite::WIRETYPE_FIXED32 &&
          WireFormatLite::ReadPrimitive<uint32_t, WireFormatLite::TYPE_FIXED32>(
              in, &ui32)) {
        *value = ui32;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_FIXED64:
      if (wire_type == WireFormatLite::WIRETYPE_FIXED64 &&
          WireFormatLite::ReadPrimitive<uint64_t, WireFormatLite::TYPE_FIXED64>(
              in, &ui64)) {
        *value = ui64;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_SFIXED32:
      if (wire_type == WireFormatLite::WIRETYPE_FIXED32 &&
          WireFormatLite::ReadPrimitive<int32_t, WireFormatLite::TYPE_SFIXED32>(
              in, &i32)) {
        *value = i32;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_SFIXED64:
      if (wire_type == WireFormatLite::WIRETYPE_FIXED64 &&
          WireFormatLite::ReadPrimitive<int64_t, WireFormatLite::TYPE_SFIXED64>(
              in, &i64)) {
        *value = i64;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_BOOL:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<bool, WireFormatLite::TYPE_BOOL>(in,
                                                                         &b)) {
        *value = b;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_ENUM:
      if (wire_type == WireFormatLite::WIRETYPE_VARINT &&
          WireFormatLite::ReadPrimitive<int32_t, WireFormatLite::TYPE_ENUM>(
              in, &i32)) {
        *value = i32;
        return true;
      }
      return false;

    case WireFormatLite::TYPE_FLOAT:
      if (wire_type == WireFormatLite::WIRETYPE_FIXED32 &&
          WireFormatLite::ReadPrimitive<float, WireFormatLite::TYPE_FLOAT>(
              in, &f)) {
        *value = f;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_DOUBLE:
      if (wire_type == WireFormatLite::WIRETYPE_FIXED64 &&
          WireFormatLite::ReadPrimitive<double, WireFormatLite::TYPE_DOUBLE>(
              in, &d)) {
        *value = d;
        return true;
      }
      return false;
    case WireFormatLite::TYPE_BYTES:
    case WireFormatLite::TYPE_MESSAGE:
    case WireFormatLite::TYPE_STRING: {
      std::string s;
      if (WireFormatLite::ReadString(in, &s)) {
        *value = std::move(s);
        return true;
      }
      return false;
    }
    case WireFormatLite::TYPE_GROUP: {
      const uint32_t start_position = in->CurrentPosition();
      if (!WireFormatLite::SkipField(in, tag_and_type)) return false;
      const int end_group_tag_size =
          google::protobuf::io::CodedOutputStream::VarintSize32(WireFormatLite::MakeTag(
              WireFormatLite::GetTagFieldNumber(tag_and_type),
              WireFormatLite::WIRETYPE_END_GROUP));
      const int group_size =
          in->CurrentPosition() - start_position - end_group_tag_size;
      *value = bytes.substr(start_position, group_size);
      return true;
    }
  }
}

// Ideally we would log the type and maybe FieldFormat here, but that would
// require passing a ProductMode which is overkill.
static std::string MakeReadValueErrorReason(
    const google::protobuf::FieldDescriptor* field_descriptor, FieldFormat::Format format,
    int64_t v) {
  std::string reason = absl::StrCat(
      "Corrupted protocol buffer: Failed to interpret value for field ",
      field_descriptor->full_name());
  if (format != FieldFormat::DEFAULT_FORMAT) {
    absl::StrAppend(&reason, " with field format ",
                    FieldFormat_Format_Name(format));
  }
  absl::StrAppend(&reason, ": ", v);
  return reason;
}

namespace {

struct VisitIntegerWireValueAsInt64 {
  zetasql_base::StatusOr<int64_t> operator()(int32_t i) const { return i; }
  zetasql_base::StatusOr<int64_t> operator()(int64_t i) const { return i; }
  zetasql_base::StatusOr<int64_t> operator()(uint32_t i) const { return i; }
  zetasql_base::StatusOr<int64_t> operator()(uint64_t i) const {
    return absl::bit_cast<int64_t>(i);
  }
  template <typename T>
  zetasql_base::StatusOr<int64_t> operator()(T) const {
    ZETASQL_RET_CHECK_FAIL() << "Unexpected type kind " << typeid(T).name()
                     << " in IntegerWireValueAsInt64()";
  }
};

// Same as 'value.ToInt64()', except uint64s are casted to int64_t, and bools and
// enums are not supported.
zetasql_base::StatusOr<int64_t> IntegerWireValueAsInt64(const WireValueType& value) {
  return absl::visit(VisitIntegerWireValueAsInt64(), value);
}

}  // namespace

// Translate the proto field value 'wire_value' (obtained from ReadWireValue())
// to the corresponding zetasql Value. 'type' must correspond to
// 'field_descriptor' and 'format'.
static zetasql_base::StatusOr<Value> TranslateWireValue(
    const WireValueType& wire_value,
    const google::protobuf::FieldDescriptor* field_descriptor, FieldFormat::Format format,
    const Type* type) {
  ZETASQL_RET_CHECK(!type->IsArray());

  switch (type->kind()) {
    case TYPE_INT32: {
      const int32_t* const value = absl::get_if<int32_t>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Int32(*value);
    }
    case TYPE_INT64: {
      const int64_t* const value = absl::get_if<int64_t>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Int64(*value);
    }
    case TYPE_DATE: {
      ZETASQL_ASSIGN_OR_RETURN(const int64_t v, IntegerWireValueAsInt64(wire_value));
      int32_t decoded_date;
      bool is_null;
      const zetasql_base::Status status =
          functions::DecodeFormattedDate(v, format, &decoded_date, &is_null);
      if (ABSL_PREDICT_TRUE(status.ok())) {
        if (is_null) {
          return Value::NullDate();
        } else if (decoded_date >= types::kDateMin &&
                   decoded_date <= types::kDateMax) {
          return Value::Date(decoded_date);
        }
      }
      return zetasql_base::OutOfRangeErrorBuilder()
             << MakeReadValueErrorReason(field_descriptor, format, v);
    }
    case TYPE_TIMESTAMP: {
      ZETASQL_ASSIGN_OR_RETURN(const int64_t v, IntegerWireValueAsInt64(wire_value));
      int64_t adjusted_timestamp;
      if (ABSL_PREDICT_FALSE(
              !Int64ToAdjustedTimestampInt64(format, v, &adjusted_timestamp)
                   .ok()) ||
          ABSL_PREDICT_FALSE(adjusted_timestamp < types::kTimestampMin) ||
          ABSL_PREDICT_FALSE(adjusted_timestamp > types::kTimestampMax)) {
        // Adjustment for precision caused arithmetic overflow, or is out of
        // bounds.
        return zetasql_base::OutOfRangeErrorBuilder()
               << MakeReadValueErrorReason(field_descriptor, format, v);
      } else {
        return Value::TimestampFromUnixMicros(adjusted_timestamp);
      }
    }
    case TYPE_TIME: {
      ZETASQL_ASSIGN_OR_RETURN(const int64_t v, IntegerWireValueAsInt64(wire_value));
      TimeValue time = TimeValue::FromPacked64Micros(v);
      if (ABSL_PREDICT_TRUE(time.IsValid())) {
        return Value::Time(time);
      } else {
        return zetasql_base::OutOfRangeErrorBuilder()
               << MakeReadValueErrorReason(field_descriptor, format, v);
      }
    }
    case TYPE_DATETIME: {
      ZETASQL_ASSIGN_OR_RETURN(const int64_t v, IntegerWireValueAsInt64(wire_value));
      DatetimeValue datetime = DatetimeValue::FromPacked64Micros(v);
      if (ABSL_PREDICT_TRUE(datetime.IsValid())) {
        return Value::Datetime(datetime);
      } else {
        return zetasql_base::OutOfRangeErrorBuilder()
               << MakeReadValueErrorReason(field_descriptor, format, v);
      }
    }
    case TYPE_UINT32: {
      const uint32_t* const value = absl::get_if<uint32_t>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Uint32(*value);
    }
    case TYPE_UINT64: {
      const uint64_t* const value = absl::get_if<uint64_t>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Uint64(*value);
    }
    case TYPE_BOOL: {
      const bool* const value = absl::get_if<bool>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Bool(*value);
    }
    case TYPE_FLOAT: {
      const float* const value = absl::get_if<float>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Float(*value);
    }
    case TYPE_DOUBLE: {
      const double* const value = absl::get_if<double>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Double(*value);
    }
    case TYPE_ENUM: {
      const int32_t* const value = absl::get_if<int32_t>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      const Value enum_value = Value::Enum(type->AsEnum(), *value);
      if (ABSL_PREDICT_FALSE(!enum_value.is_valid())) {
        return zetasql_base::OutOfRangeErrorBuilder()
               << MakeReadValueErrorReason(field_descriptor, format, *value);
      }
      return enum_value;
    }
    case TYPE_STRING: {
      const std::string* const value = absl::get_if<std::string>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::String(*value);
    }
    case TYPE_BYTES: {
      const std::string* const value = absl::get_if<std::string>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);
      return Value::Bytes(*value);
    }
    case TYPE_PROTO: {
      const std::string* const value = absl::get_if<std::string>(&wire_value);
      ZETASQL_RET_CHECK_NE(value, nullptr);

      return Value::Proto(type->AsProto(), *value);
    }
    case TYPE_ARRAY:
    case TYPE_STRUCT:
    default:
      ZETASQL_RET_CHECK_FAIL() << "Unexpected type kind: " << type->kind();
  }
}

inline bool IsPackedWireType(uint32_t tag) {
  return WireFormatLite::GetTagWireType(tag) ==
      WireFormatLite::WIRETYPE_LENGTH_DELIMITED;
}

using PackedValuesVector = absl::InlinedVector<WireValueType, 8>;

// Returns true on success.
static bool ReadPackedWireValues(int tag_number,
                                 google::protobuf::FieldDescriptor::Type field_type,
                                 google::protobuf::io::CodedInputStream* in,
                                 PackedValuesVector* values) {
  int length;
  if (!in->ReadVarintSizeAsInt(&length) || length <= 0) {
    return false;
  }
  // Only primitive numeric/enum values can be packed.
  std::string unused_bytes;
  google::protobuf::io::CodedInputStream::Limit limit = in->PushLimit(length);
  uint32_t tag_and_type = WireFormatLite::MakeTag(
      tag_number, WireFormatLite::WireTypeForFieldType(
                      static_cast<WireFormatLite::FieldType>(
                          absl::implicit_cast<int>(field_type))));
  while (in->BytesUntilLimit() > 0) {
    WireValueType value;
    if (!ReadWireValue(field_type, tag_and_type, unused_bytes, in, &value)) {
      return false;
    }
    values->push_back(std::move(value));
  }
  in->PopLimit(limit);
  return true;
}

// Optimized version of ReadProtoFields where only one field is being fetched.
static zetasql_base::StatusOr<Value> ReadSingularProtoField(
    const ProtoFieldInfo& field_info,
    const std::string& bytes) {
  const int field_info_tag = field_info.descriptor->number();

  // The elements we have seen for 'field_info'. Only used if
  // 'field_info.get_has_bit' is false.
  absl::InlinedVector<Value, 8> elements;
  const bool is_packable = field_info.descriptor->is_packable();
  uint32_t tag_and_type;
  google::protobuf::io::ArrayInputStream cord_stream(bytes.data(), bytes.size());
  google::protobuf::io::CodedInputStream in(&cord_stream);
  while (0 < (tag_and_type = in.ReadTag())) {
    const int tag_number = WireFormatLite::GetTagFieldNumber(tag_and_type);
    if (tag_number != field_info_tag) {
      if (ABSL_PREDICT_TRUE(WireFormatLite::SkipField(&in, tag_and_type))) {
        continue;
      }
      return ::zetasql_base::OutOfRangeErrorBuilder()
             << "Corrupted protocol buffer: "
             << "Failed to skip field with tag number " << tag_number << " in "
             << field_info.descriptor->containing_type()->full_name();
    }

    PackedValuesVector wire_values;
    // Protocol buffer parsers must be able to parse repeated fields that were
    // compiled as packed as if they were not packed, and vice versa.  Both
    // packed and non-packed field occurrences may appear within the same
    // message.
    if (is_packable && IsPackedWireType(tag_and_type)) {
      if (ABSL_PREDICT_FALSE(!ReadPackedWireValues(
              field_info.descriptor->number(), field_info.descriptor->type(),
              &in, &wire_values))) {
        return ::zetasql_base::OutOfRangeErrorBuilder()
               << "Corrupted protocol buffer: "
               << "Failed to read packed elements for field "
               << field_info.descriptor->full_name();
      }
    } else {
      WireValueType wire_value;
      if (ABSL_PREDICT_FALSE(!ReadWireValue(field_info.descriptor->type(),
                                            tag_and_type, bytes, &in,
                                            &wire_value))) {
        return zetasql_base::OutOfRangeErrorBuilder()
               << "Corrupted protocol buffer: Failed to read value for field "
               << field_info.descriptor->full_name();
      }
      wire_values.push_back(std::move(wire_value));
    }
    ZETASQL_RET_CHECK(!wire_values.empty());

    if (field_info.get_has_bit) {
      return Value::Bool(true);
    }
    for (const WireValueType& wire_value : wire_values) {
      const Type* element_type =
          field_info.type->IsArray()
              ? field_info.type->AsArray()->element_type()
              : field_info.type;
      ZETASQL_ASSIGN_OR_RETURN(Value element,
                       TranslateWireValue(wire_value, field_info.descriptor,
                                          field_info.format, element_type));
      elements.push_back(std::move(element));
    }
  }

  // Now that we have read all of the values we care about, use them to populate
  // the return value.
  if (field_info.get_has_bit) {
    return Value::Bool(false);
  }
  ZETASQL_RET_CHECK_EQ(field_info.type->IsArray(),
               field_info.descriptor->is_repeated());
  if (field_info.type->IsArray()) {
    return Value::Array(field_info.type->AsArray(), elements);
  }
  if (elements.empty()) {
    if (ABSL_PREDICT_FALSE(field_info.descriptor->is_required())) {
      return zetasql_base::OutOfRangeErrorBuilder()
             << "Protocol buffer missing required field "
             << field_info.descriptor->full_name();
    } else {
      return field_info.default_value;
    }
  }
  return std::move(elements.back());
}

namespace {

// Maps a tag number to all ProtoFieldInfos indexes with that tag number.
using FieldInfoMap = absl::flat_hash_map<int, std::vector<int>>;

// Maps a ProtoFieldInfo (by its index) to the corresponding Values we have
// seen for it, with errors for failure to convert wire values to the
// appropriate FieldFormats.
using ElementValueList = std::vector<std::vector<zetasql_base::StatusOr<Value>>>;

}  // namespace

zetasql_base::Status ReadProtoFields(
    absl::Span<const ProtoFieldInfo* const> field_infos,
    const std::string& bytes,
    ProtoFieldValueList* field_value_list) {
  const bool use_optimization =
      field_infos.size() == 1 &&
      absl::GetFlag(FLAGS_zetasql_read_proto_field_optimized_path);

  if (use_optimization) {
    ZETASQL_ASSIGN_OR_RETURN(zetasql_base::StatusOr<Value> value,
                     ReadSingularProtoField(*field_infos[0], bytes));
    field_value_list->push_back(std::move(value));
    return zetasql_base::OkStatus();
  }

  field_value_list->resize(field_infos.size());

  FieldInfoMap field_info_map;
  for (int i = 0; i < field_infos.size(); ++i) {
    const ProtoFieldInfo* field_info = field_infos[i];
    field_info_map[field_info->descriptor->number()].push_back(i);
  }

  // If get_has_bit is true, this is either empty or contains a single
  // Value::Bool(true).
  ElementValueList element_value_list(field_infos.size());
  ZETASQL_RET_CHECK(!field_infos.empty());
  const google::protobuf::FieldDescriptor* some_field = field_infos[0]->descriptor;
    uint32_t tag_and_type;
    google::protobuf::io::ArrayInputStream cord_stream(bytes.data(), bytes.size());
    google::protobuf::io::CodedInputStream in(&cord_stream);
    while (0 < (tag_and_type = in.ReadTag())) {
      const int tag_number = WireFormatLite::GetTagFieldNumber(tag_and_type);
      const std::vector<int>* info_idxs =
          zetasql_base::FindOrNull(field_info_map, tag_number);
      if (info_idxs == nullptr) {
        if (ABSL_PREDICT_TRUE(WireFormatLite::SkipField(&in, tag_and_type))) {
          continue;
        }
        return ::zetasql_base::OutOfRangeErrorBuilder()
               << "Corrupted protocol buffer: "
               << "Failed to skip field with tag number " << tag_number
               << " in " << some_field->containing_type()->full_name();
      }
      // All of the field descriptors must come from the same
      // google::protobuf::Descriptor, so for a particular tag number, they are all the
      // same.
      ZETASQL_RET_CHECK(!info_idxs->empty());
      const google::protobuf::FieldDescriptor* descriptor =
          field_infos[(*info_idxs)[0]]->descriptor;

      PackedValuesVector wire_values;
      // Protocol buffer parsers must be able to parse repeated fields that were
      // compiled as packed as if they were not packed, and vice versa.  Both
      // packed and non-packed field occurrences may appear within the same
      // message.
      if (descriptor->is_packable() && IsPackedWireType(tag_and_type)) {
        if (ABSL_PREDICT_FALSE(!ReadPackedWireValues(
                descriptor->number(), descriptor->type(), &in, &wire_values))) {
          return ::zetasql_base::OutOfRangeErrorBuilder()
                 << "Corrupted protocol buffer: "
                 << "Failed to read packed elements for field "
                 << descriptor->full_name();
        }
      } else {
        WireValueType wire_value;
        if (ABSL_PREDICT_FALSE(!ReadWireValue(descriptor->type(), tag_and_type,
                                              bytes, &in, &wire_value))) {
          return zetasql_base::OutOfRangeErrorBuilder()
                 << "Corrupted protocol buffer: Failed to read value for field "
                 << descriptor->full_name();
        }
        wire_values.push_back(std::move(wire_value));
      }
      ZETASQL_RET_CHECK(!wire_values.empty());

      for (int i = 0; i < info_idxs->size(); ++i) {
        const int idx = (*info_idxs)[i];
        const ProtoFieldInfo* info = field_infos[idx];
        std::vector<zetasql_base::StatusOr<Value>>& elements = element_value_list[idx];

        if (info->get_has_bit) {
          if (elements.empty()) {
            elements.push_back(Value::Bool(true));
          }
        } else {
          const Type* element_type = info->type->IsArray()
                                         ? info->type->AsArray()->element_type()
                                         : info->type;
          for (const WireValueType& wire_value : wire_values) {
            elements.push_back(TranslateWireValue(wire_value, descriptor,
                                                  info->format, element_type));
          }
        }
      }
    }

  // Now that we have read all of the values we care about, use them to populate
  // 'field_value_list'.
  for (int i = 0; i < field_infos.size(); ++i) {
    const ProtoFieldInfo* info = field_infos[i];
    std::vector<zetasql_base::StatusOr<Value>>& values = element_value_list[i];
    if (info->get_has_bit) {
      (*field_value_list)[i] = Value::Bool(!values.empty());
    } else {
      zetasql_base::StatusOr<Value> new_value;

      ZETASQL_RET_CHECK_EQ(info->type->IsArray(), info->descriptor->is_repeated());
      if (info->type->IsArray()) {
        std::vector<Value> element_values;
        bool success = true;
        for (zetasql_base::StatusOr<Value>& value : values) {
          if (ABSL_PREDICT_FALSE(!value.ok())) {
            success = false;
            new_value = value.status();
            break;
          }
          element_values.push_back(value.ValueOrDie());
        }

        if (success) {
          new_value = Value::Array(info->type->AsArray(), element_values);
        }
      } else if (values.empty()) {
        if (ABSL_PREDICT_FALSE(info->descriptor->is_required())) {
          new_value = zetasql_base::Status(zetasql_base::StatusCode::kOutOfRange,
                                   "Protocol buffer missing required field " +
                                       info->descriptor->full_name());
        } else {
          new_value = info->default_value;
        }
      } else {
        new_value = std::move(values.back());
      }

      (*field_value_list)[i] = std::move(new_value);
    }
  }

  return zetasql_base::OkStatus();
}

zetasql_base::Status ReadProtoField(
    const google::protobuf::FieldDescriptor* field_descr, FieldFormat::Format format,
    const Type* type, const Value& default_value, bool get_has_bit,
    const std::string& bytes,
    Value* output_value) {
  ProtoFieldInfo info;
  info.descriptor = field_descr;
  info.format = format;
  info.type = type;
  info.default_value = default_value;
  info.get_has_bit = get_has_bit;

  ProtoFieldValueList field_value_list;
  ZETASQL_RETURN_IF_ERROR(ReadProtoFields({&info}, bytes, &field_value_list));
  ZETASQL_RET_CHECK_EQ(field_value_list.size(), 1);
  const zetasql_base::StatusOr<Value>& status_or_value = field_value_list[0];
  ZETASQL_RETURN_IF_ERROR(status_or_value.status());
  *output_value = status_or_value.ValueOrDie();
  return zetasql_base::OkStatus();
}

zetasql_base::Status ReadProtoField(
    const google::protobuf::FieldDescriptor* field_descr, FieldFormat::Format format,
    const Type* type, const Value& default_value,
    const std::string& bytes,
    Value* output_value) {
  return ReadProtoField(field_descr, format, type, default_value, false, bytes,
                        output_value);
}

zetasql_base::Status ProtoHasField(
    int32_t field_tag,
    const std::string& bytes,
    bool* has_field) {
  *has_field = false;
    google::protobuf::io::ArrayInputStream cord_stream(bytes.data(), bytes.size());
    google::protobuf::io::CodedInputStream in(&cord_stream);
    uint32_t tag_and_type;
    while (0 < (tag_and_type = in.ReadTag())) {
      if (field_tag == WireFormatLite::GetTagFieldNumber(tag_and_type)) {
        // Stop once we find one instance of the field.
        // We have no special treatment for repeated fields here.
        *has_field = true;
        break;
      }
      if (!WireFormatLite::SkipField(&in, tag_and_type)) {
        return zetasql_base::Status(zetasql_base::StatusCode::kOutOfRange,
                            "Corrupted protocol buffer");
      }
    }
  return ::zetasql_base::OkStatus();
}

}  // namespace zetasql
