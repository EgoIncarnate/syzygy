// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/refinery/analyzers/stack_frame_analyzer_impl.h"

#include <vector>

#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "syzygy/pe/dia_util.h"
#include "syzygy/refinery/core/address.h"
#include "syzygy/refinery/process_state/refinery.pb.h"
#include "syzygy/refinery/types/type.h"
#include "third_party/cci/files/cvinfo.h"

namespace refinery {

namespace {

// We observe that CV_ALLREG_VFRAME register-relative locations actually refer
// to the parent frame's value.
bool GetRegRelLocationRegisterValue(StackFrameRecordPtr frame_record,
                                    uint32_t register_id,
                                    uint32_t* register_value) {
  DCHECK(frame_record); DCHECK(register_value);

  const RegisterInformation& context = frame_record->data().register_info();
  // Note: requests for CV_ALLREG_VFRAME are served with parent_allreg_vframe.
  if (register_id == CV_ALLREG_VFRAME && context.has_parent_allreg_vframe()) {
    *register_value = context.parent_allreg_vframe();
    return true;
  }

  return false;
}

bool GetTypeName(IDiaSymbol* data, base::string16* type_name) {
  DCHECK(data); DCHECK(type_name);

  base::win::ScopedComPtr<IDiaSymbol> type;
  if (!pe::GetSymType(data, &type))
    return false;

  // TODO(manzagop): support naming basic types, arrays, pointers, etc.
  if (!pe::GetSymName(type.get(), type_name))
    *type_name = base::ASCIIToUTF16("<unknown-type=name>");

  return true;
}

}  // namespace

StackFrameDataAnalyzer::StackFrameDataAnalyzer(
    StackFrameRecordPtr frame_record,
    scoped_refptr<TypeNameIndex> typename_index,
    ProcessState* process_state)
    : frame_record_(frame_record),
      typename_index_(typename_index),
      process_state_(process_state) {
  DCHECK(frame_record.get());
  DCHECK(typename_index.get());
  DCHECK(process_state);
}

bool StackFrameDataAnalyzer::Analyze(IDiaSymbol* data) {
  DCHECK(data);
  DCHECK(pe::IsSymTag(data, SymTagData));

  // Restrict to local variables, parameters and this pointers.
  // TODO(manzagop): processing for other kinds, eg DataIsMember?
  DataKind data_kind = DataIsUnknown;
  if (!pe::GetDataKind(data, &data_kind))
    return false;
  if (data_kind != DataIsLocal && data_kind != DataIsParam &&
      data_kind != DataIsObjectPtr) {
    return true;  // Ignore these for now.
  }

  // Get the data's information: name, type name and address range.
  base::string16 data_name;
  if (!pe::GetSymName(data, &data_name))
    return false;

  base::string16 type_name;
  if (!GetTypeName(data, &type_name))
    return false;

  AddressRange range;
  if (!GetAddressRange(data, &range))
    return false;
  // Note: successfully returning an invalid address range means the location
  // type is not yet supported.
  // TODO(manzagop): fully support location types and remove this.
  if (!range.IsValid())
    return true;

  // Add the typed block to the process state's typed block layer.
  // TODO(manzagop): handle CV qualifiers.
  return AddTypedBlockRecord(range, data_name, type_name, process_state_);
}

bool StackFrameDataAnalyzer::GetAddressRange(IDiaSymbol* data,
                                             AddressRange* range) {
  DCHECK(data); DCHECK(range);

  AddressRange address_range;
  *range = address_range;

  // Restrict to register relative locations: register id and offset.
  // TODO(manzagop): support other location types, eg enregistered.
  LocationType location_type = LocIsNull;
  if (!pe::GetLocationType(data, &location_type))
    return false;
  if (location_type != LocIsRegRel)
    return true;

  // Register-relative: determine location.
  uint32_t register_id = 0U;
  if (!pe::GetRegisterId(data, &register_id))
    return false;
  ptrdiff_t register_offset = 0;
  if (!pe::GetSymOffset(data, &register_offset))
    return false;

  // Get the data's type from the type_repository.
  // TODO(manzagop): stop relying on type name for retrieving the type once DIA
  // is no longer used and we have a stable id.
  base::string16 type_name;
  if (!GetTypeName(data, &type_name))
    return false;

  // TODO(manzagop): handle basic types, pointers and arrays. This requires
  // figuring out their names.
  base::win::ScopedComPtr<IDiaSymbol> dia_type;
  if (!pe::GetSymType(data, &dia_type))
    return false;
  enum SymTagEnum sym_tag_type = SymTagNull;
  if (!pe::GetSymTag(dia_type.get(), &sym_tag_type))
    return false;
  if (sym_tag_type != SymTagUDT)
    return true;

  // Retrieve symbol information.
  std::vector<TypePtr> matching_types;
  typename_index_->GetTypes(type_name, &matching_types);
  if (matching_types.empty()) {
    LOG(INFO) << "Type " << type_name << " was not found. Skipping.";
    return true;
  } else if (matching_types.size() > 1) {
    LOG(INFO) << "Type name " << type_name << " is ambiguous. Skipping.";
    return true;
  }
  DCHECK_EQ(1U, matching_types.size());
  TypePtr type = matching_types[0];

  // Figure out the data's range.
  uint32_t register_value = 0U;
  if (!GetRegRelLocationRegisterValue(frame_record_, register_id,
                                      &register_value)) {
    LOG(ERROR) << base::StringPrintf(
        "Failed to retrieve register value (%d). Skipping data.", register_id);
    return true;
  }

  // TODO(manzagop): check validity of operation.
  Address data_va = register_value + register_offset;
  address_range.set_start(data_va);
  address_range.set_size(type->size());
  if (!address_range.IsValid())
    return false;

  *range = address_range;
  return true;
}

}  // namespace refinery