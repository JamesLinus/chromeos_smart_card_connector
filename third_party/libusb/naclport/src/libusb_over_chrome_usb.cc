// Copyright 2016 Google Inc.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "libusb_over_chrome_usb.h"

#include <stdlib.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include <ppapi/cpp/var_array_buffer.h>

#include <google_smart_card_common/logging/logging.h>
#include <google_smart_card_common/pp_var_utils/construction.h>
#include <google_smart_card_common/pp_var_utils/extraction.h>

#include "libusb_opaque_types.h"

// This arbitrary chosen constant is used as a stub for the device bus number
// (as the chrome.usb API does not provide means of retrieving this).
const uint8_t kFakeDeviceBusNumber = 42;

//
// Bit mask values for the bmAttributes field of the libusb_config_descriptor
// structure.
//

const uint8_t kLibusbConfigDescriptorBmAttributesRemoteWakeup = 1 << 5;
const uint8_t kLibusbConfigDescriptorBmAttributesSelfPowered = 1 << 6;

//
// Positions of the first non-zero bits in the libusb mask constants.
//

const int kLibusbTransferTypeMaskShift = 0;
static_assert(
    !(LIBUSB_TRANSFER_TYPE_MASK & ((1 << kLibusbTransferTypeMaskShift) - 1)),
    "kLibusbTransferTypeMaskShift constant is wrong");
static_assert(
    (LIBUSB_TRANSFER_TYPE_MASK >> kLibusbTransferTypeMaskShift) & 1,
    "kLibusbTransferTypeMaskShift constant is wrong");

const int kLibusbIsoSyncTypeMaskShift = 2;
static_assert(
    !(LIBUSB_ISO_SYNC_TYPE_MASK & ((1 << kLibusbIsoSyncTypeMaskShift) - 1)),
    "kLibusbIsoSyncTypeMaskShift constant is wrong");
static_assert(
    (LIBUSB_ISO_SYNC_TYPE_MASK >> kLibusbIsoSyncTypeMaskShift) & 1,
    "kLibusbIsoSyncTypeMaskShift constant is wrong");

const int kLibusbIsoUsageTypeMaskShift = 4;
static_assert(
    !(LIBUSB_ISO_USAGE_TYPE_MASK & ((1 << kLibusbIsoUsageTypeMaskShift) - 1)),
    "kLibusbIsoUsageTypeMaskShift constant is wrong");
static_assert(
    (LIBUSB_ISO_USAGE_TYPE_MASK >> kLibusbIsoUsageTypeMaskShift) & 1,
    "kLibusbIsoUsageTypeMaskShift constant is wrong");

// Mask for libusb_request_recipient bits in bmRequestType field of the
// libusb_control_setup structure.
const int kLibusbRequestRecipientMask =
    LIBUSB_RECIPIENT_DEVICE | LIBUSB_RECIPIENT_INTERFACE |
    LIBUSB_RECIPIENT_ENDPOINT | LIBUSB_RECIPIENT_OTHER;

// Mask for libusb_request_type bits in bmRequestType field of the
// libusb_control_setup structure.
const int kLibusbRequestTypeMask =
    LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_REQUEST_TYPE_CLASS |
    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_REQUEST_TYPE_RESERVED;

namespace google_smart_card {

namespace {

std::unique_ptr<uint8_t> CopyRawData(const uint8_t* data, size_t byte_count) {
  std::unique_ptr<uint8_t> result(new uint8_t[byte_count]);
  std::copy(data, data + byte_count, result.get());
  return result;
}

std::unique_ptr<uint8_t> CopyRawData(const pp::VarArrayBuffer& data) {
  const std::vector<uint8_t> data_vector = VarAs<std::vector<uint8_t>>(data);
  if (data_vector.empty())
    return nullptr;
  return CopyRawData(&data_vector[0], data_vector.size());
}

}  // namespace

LibusbOverChromeUsb::LibusbOverChromeUsb(
    chrome_usb::ApiBridge* chrome_usb_api_bridge)
    : chrome_usb_api_bridge_(chrome_usb_api_bridge),
      default_context_(new libusb_context) {
  GOOGLE_SMART_CARD_CHECK(chrome_usb_api_bridge_);
}

LibusbOverChromeUsb::~LibusbOverChromeUsb() = default;

ssize_t LibusbOverChromeUsb::LibusbGetDeviceList(
    libusb_context* context, libusb_device*** device_list) {
  GOOGLE_SMART_CARD_CHECK(device_list);

  context = SubstituteDefaultContextIfNull(context);

  const RequestResult<chrome_usb::GetDevicesResult> result =
      chrome_usb_api_bridge_->GetDevices(chrome_usb::GetDevicesOptions());
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbGetDeviceList request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  const std::vector<chrome_usb::Device>& chrome_usb_devices =
      result.payload().devices;

  *device_list = new libusb_device*[chrome_usb_devices.size() + 1];
  for (size_t index = 0; index < chrome_usb_devices.size(); ++index) {
    (*device_list)[index] = new libusb_device(
        context, chrome_usb_devices[index]);
  }

  // The resulting list must be NULL-terminated according to the libusb
  // documentation.
  (*device_list)[chrome_usb_devices.size()] = nullptr;

  return chrome_usb_devices.size();
}

void LibusbOverChromeUsb::LibusbFreeDeviceList(
    libusb_device** device_list, int unref_devices) {
  if (!device_list)
    return;
  if (unref_devices) {
    for (size_t index = 0; device_list[index]; ++index)
      LibusbUnrefDevice(device_list[index]);
  }
  delete[] device_list;
}

libusb_device* LibusbOverChromeUsb::LibusbRefDevice(libusb_device* device) {
  GOOGLE_SMART_CARD_CHECK(device);

  device->AddReference();

  return device;
}

void LibusbOverChromeUsb::LibusbUnrefDevice(libusb_device* device) {
  GOOGLE_SMART_CARD_CHECK(device);

  device->RemoveReference();
}

namespace {

uint8_t ChromeUsbEndpointDescriptorTypeToLibusbMask(
    chrome_usb::EndpointDescriptorType value) {
  switch (value) {
    case chrome_usb::EndpointDescriptorType::kControl:
      return LIBUSB_TRANSFER_TYPE_CONTROL << kLibusbTransferTypeMaskShift;
    case chrome_usb::EndpointDescriptorType::kInterrupt:
      return LIBUSB_TRANSFER_TYPE_INTERRUPT << kLibusbTransferTypeMaskShift;
    case chrome_usb::EndpointDescriptorType::kIsochronous:
      return LIBUSB_TRANSFER_TYPE_ISOCHRONOUS << kLibusbTransferTypeMaskShift;
    case chrome_usb::EndpointDescriptorType::kBulk:
      return LIBUSB_TRANSFER_TYPE_BULK << kLibusbTransferTypeMaskShift;
    default:
      GOOGLE_SMART_CARD_NOTREACHED;
  }
}

uint8_t ChromeUsbEndpointDescriptorSynchronizationToLibusbMask(
    chrome_usb::EndpointDescriptorSynchronization value) {
  switch (value) {
    case chrome_usb::EndpointDescriptorSynchronization::kAsynchronous:
      return LIBUSB_ISO_SYNC_TYPE_ASYNC << kLibusbIsoSyncTypeMaskShift;
    case chrome_usb::EndpointDescriptorSynchronization::kAdaptive:
      return LIBUSB_ISO_SYNC_TYPE_ADAPTIVE << kLibusbIsoSyncTypeMaskShift;
    case chrome_usb::EndpointDescriptorSynchronization::kSynchronous:
      return LIBUSB_ISO_SYNC_TYPE_SYNC << kLibusbIsoSyncTypeMaskShift;
    default:
      GOOGLE_SMART_CARD_NOTREACHED;
  }
}

uint8_t ChromeUsbEndpointDescriptorUsageToLibusbMask(
    chrome_usb::EndpointDescriptorUsage value) {
  switch (value) {
    case chrome_usb::EndpointDescriptorUsage::kData:
      return LIBUSB_ISO_USAGE_TYPE_DATA << kLibusbIsoUsageTypeMaskShift;
    case chrome_usb::EndpointDescriptorUsage::kFeedback:
      return LIBUSB_ISO_USAGE_TYPE_FEEDBACK << kLibusbIsoUsageTypeMaskShift;
    case chrome_usb::EndpointDescriptorUsage::kExplicitFeedback:
      return LIBUSB_ISO_USAGE_TYPE_IMPLICIT << kLibusbIsoUsageTypeMaskShift;
    default:
      GOOGLE_SMART_CARD_NOTREACHED;
  }
}

void FillLibusbEndpointDescriptor(
    const chrome_usb::EndpointDescriptor& chrome_usb_descriptor,
    libusb_endpoint_descriptor* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  std::memset(result, 0, sizeof(libusb_endpoint_descriptor));

  result->bLength = sizeof(libusb_endpoint_descriptor);

  result->bDescriptorType = LIBUSB_DT_ENDPOINT;

  result->bEndpointAddress = chrome_usb_descriptor.address;

  result->bmAttributes |= ChromeUsbEndpointDescriptorTypeToLibusbMask(
      chrome_usb_descriptor.type);
  if (chrome_usb_descriptor.type ==
      chrome_usb::EndpointDescriptorType::kIsochronous) {
    GOOGLE_SMART_CARD_CHECK(chrome_usb_descriptor.synchronization);
    result->bmAttributes |=
        ChromeUsbEndpointDescriptorSynchronizationToLibusbMask(
            *chrome_usb_descriptor.synchronization);
    GOOGLE_SMART_CARD_CHECK(chrome_usb_descriptor.usage);
    result->bmAttributes |= ChromeUsbEndpointDescriptorUsageToLibusbMask(
        *chrome_usb_descriptor.usage);
  }

  result->wMaxPacketSize = chrome_usb_descriptor.maximum_packet_size;

  if (chrome_usb_descriptor.polling_interval)
    result->bInterval = *chrome_usb_descriptor.polling_interval;

  result->extra = CopyRawData(chrome_usb_descriptor.extra_data).release();

  result->extra_length = chrome_usb_descriptor.extra_data.ByteLength();
}

void FillLibusbInterfaceDescriptor(
    const chrome_usb::InterfaceDescriptor& chrome_usb_descriptor,
    libusb_interface_descriptor* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  std::memset(result, 0, sizeof(libusb_interface_descriptor));

  result->bLength = sizeof(libusb_interface_descriptor);

  result->bDescriptorType = LIBUSB_DT_INTERFACE;

  result->bInterfaceNumber = chrome_usb_descriptor.interface_number;

  result->bNumEndpoints = chrome_usb_descriptor.endpoints.size();

  result->bInterfaceClass = chrome_usb_descriptor.interface_class;

  result->bInterfaceSubClass = chrome_usb_descriptor.interface_subclass;

  result->bInterfaceProtocol = chrome_usb_descriptor.interface_protocol;

  result->endpoint = new libusb_endpoint_descriptor[
      chrome_usb_descriptor.endpoints.size()];
  for (size_t index = 0;
       index < chrome_usb_descriptor.endpoints.size();
       ++index) {
    FillLibusbEndpointDescriptor(
        chrome_usb_descriptor.endpoints[index],
        const_cast<libusb_endpoint_descriptor*>(&result->endpoint[index]));
  }

  result->extra = CopyRawData(chrome_usb_descriptor.extra_data).release();

  result->extra_length = chrome_usb_descriptor.extra_data.ByteLength();
}

void FillLibusbInterface(
    const chrome_usb::InterfaceDescriptor& chrome_usb_descriptor,
    libusb_interface* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  result->altsetting = new libusb_interface_descriptor[1];
  FillLibusbInterfaceDescriptor(
      chrome_usb_descriptor,
      const_cast<libusb_interface_descriptor*>(result->altsetting));

  result->num_altsetting = 1;
}

void FillLibusbConfigDescriptor(
    const chrome_usb::ConfigDescriptor& chrome_usb_descriptor,
    libusb_config_descriptor* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  std::memset(result, 0, sizeof(libusb_config_descriptor));

  result->bLength = sizeof(libusb_config_descriptor);

  result->bDescriptorType = LIBUSB_DT_CONFIG;

  result->wTotalLength = sizeof(libusb_config_descriptor);

  result->bNumInterfaces = chrome_usb_descriptor.interfaces.size();

  result->bConfigurationValue = chrome_usb_descriptor.configuration_value;

  if (chrome_usb_descriptor.remote_wakeup)
    result->bmAttributes |= kLibusbConfigDescriptorBmAttributesRemoteWakeup;
  if (chrome_usb_descriptor.self_powered)
    result->bmAttributes |= kLibusbConfigDescriptorBmAttributesSelfPowered;

  result->MaxPower = chrome_usb_descriptor.max_power;

  result->interface = new libusb_interface[
      chrome_usb_descriptor.interfaces.size()];
  for (size_t index = 0;
       index < chrome_usb_descriptor.interfaces.size();
       ++index) {
    FillLibusbInterface(
        chrome_usb_descriptor.interfaces[index],
        const_cast<libusb_interface*>(&result->interface[index]));
  }

  result->extra = CopyRawData(chrome_usb_descriptor.extra_data).release();

  result->extra_length = chrome_usb_descriptor.extra_data.ByteLength();
}

}  // namespace

int LibusbOverChromeUsb::LibusbGetActiveConfigDescriptor(
    libusb_device* device, libusb_config_descriptor** config_descriptor) {
  GOOGLE_SMART_CARD_CHECK(device);
  GOOGLE_SMART_CARD_CHECK(config_descriptor);

  const RequestResult<chrome_usb::GetConfigurationsResult> result =
      chrome_usb_api_bridge_->GetConfigurations(device->chrome_usb_device());
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbGetActiveConfigDescriptor request " <<
        "failed: " << result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  const std::vector<chrome_usb::ConfigDescriptor>& chrome_usb_configs =
      result.payload().configurations;

  *config_descriptor = nullptr;
  for (const auto& chrome_usb_config : chrome_usb_configs) {
    if (chrome_usb_config.active) {
      // Only one active configuration is expected to be returned by chrome.usb
      // API.
      GOOGLE_SMART_CARD_CHECK(!*config_descriptor);

      *config_descriptor = new libusb_config_descriptor;
      FillLibusbConfigDescriptor(chrome_usb_config, *config_descriptor);
    }
  }
  if (!*config_descriptor) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbGetActiveConfigDescriptor request " <<
        "failed: No active config descriptors were returned by chrome.usb API";
    return LIBUSB_ERROR_OTHER;
  }
  return LIBUSB_SUCCESS;
}

namespace {

void DestroyLibusbEndpointDescriptor(
    const libusb_endpoint_descriptor& endpoint_descriptor) {
  delete[] endpoint_descriptor.extra;
}

void DestroyLibusbInterfaceDescriptor(
    const libusb_interface_descriptor& interface_descriptor) {
  for (unsigned index = 0;
       index < interface_descriptor.bNumEndpoints;
       ++index) {
    DestroyLibusbEndpointDescriptor(interface_descriptor.endpoint[index]);
  }
  delete[] interface_descriptor.endpoint;

  delete[] interface_descriptor.extra;
}

void DestroyLibusbInterface(const libusb_interface& interface) {
  for (int index = 0;
       index < interface.num_altsetting;
       ++index) {
    DestroyLibusbInterfaceDescriptor(interface.altsetting[index]);
  }
  delete[] interface.altsetting;
}

void DestroyLibusbConfigDescriptor(
    const libusb_config_descriptor& config_descriptor) {
  for (uint8_t index = 0;
       index < config_descriptor.bNumInterfaces;
       ++index) {
    DestroyLibusbInterface(config_descriptor.interface[index]);
  }
  delete[] config_descriptor.interface;

  delete[] config_descriptor.extra;
}

}  // namespace

void LibusbOverChromeUsb::LibusbFreeConfigDescriptor(
    libusb_config_descriptor* config_descriptor) {
  if (!config_descriptor)
    return;
  DestroyLibusbConfigDescriptor(*config_descriptor);
  delete config_descriptor;
}

namespace {

void FillLibusbDeviceDescriptor(
    const chrome_usb::Device& chrome_usb_device,
    libusb_device_descriptor* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  std::memset(result, 0, sizeof(libusb_device_descriptor));

  result->bLength = sizeof(libusb_device_descriptor);

  result->bDescriptorType = LIBUSB_DT_DEVICE;

  result->idVendor = chrome_usb_device.vendor_id;

  result->idProduct = chrome_usb_device.product_id;

  if (chrome_usb_device.version) {
    // The "bcdDevice" field is filled only when the chrome.usb API returns the
    // corresponding information (which happens only in Chrome >= 51; refer to
    // <http://crbug.com/598825>).
    result->bcdDevice = *chrome_usb_device.version;
  }

  //
  // chrome.usb API also provides information about the product name,
  // manufacturer name and serial number. However, it's difficult to pass this
  // information to consumers here, because the corresponding
  // libusb_device_descriptor structure fields (iProduct, iManufacturer,
  // iSerialNumber) should contain not the strings themselves, but their indexes
  // instead. The string indexes, however, are not provided by chrome.usb API.
  //
  // One solution would be to use a generated string indexes here and patch the
  // inline libusb_get_string_descriptor function. But avoiding the collisions
  // of the generated string indexes with some other existing ones is difficult.
  // Moreover, this solution would still keep some incompatibility with the
  // original libusb interface, as the consumers could tried reading the strings
  // by performing the corresponding control transfers themselves instead of
  // using libusb_get_string_descriptor function - which will obviously fail.
  //
  // Another, more correct, solution would be to iterate here over all possible
  // string indexes and therefore match the strings returned by chrome.usb API
  // with their original string indexes. But this solution has an obvious
  // drawback of a big performance penalty.
  //
  // That's why it's currently decided to not populate these
  // libusb_device_descriptor structure fields at all.
  //
}

}  // namespace

int LibusbOverChromeUsb::LibusbGetDeviceDescriptor(
    libusb_device* device, libusb_device_descriptor* device_descriptor) {
  GOOGLE_SMART_CARD_CHECK(device);
  GOOGLE_SMART_CARD_CHECK(device_descriptor);

  FillLibusbDeviceDescriptor(device->chrome_usb_device(), device_descriptor);
  return LIBUSB_SUCCESS;
}

uint8_t LibusbOverChromeUsb::LibusbGetBusNumber(libusb_device* /*device*/) {
  return kFakeDeviceBusNumber;
}

uint8_t LibusbOverChromeUsb::LibusbGetDeviceAddress(libusb_device* device) {
  GOOGLE_SMART_CARD_CHECK(device);

  const int64_t device_id = device->chrome_usb_device().device;
  // FIXME(emaxx): Test this on a real device; re-think about this place.
  GOOGLE_SMART_CARD_CHECK(device_id < std::numeric_limits<uint8_t>::max());
  return static_cast<uint8_t>(device_id);
}

int LibusbOverChromeUsb::LibusbOpen(
    libusb_device* device, libusb_device_handle** device_handle) {
  GOOGLE_SMART_CARD_CHECK(device);
  GOOGLE_SMART_CARD_CHECK(device_handle);

  const RequestResult<chrome_usb::OpenDeviceResult> result =
      chrome_usb_api_bridge_->OpenDevice(device->chrome_usb_device());
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING << "LibusbOverChromeUsb::LibusbOpen " <<
        "request failed: " << result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  const chrome_usb::ConnectionHandle chrome_usb_connection_handle =
      result.payload().connection_handle;

  *device_handle = new libusb_device_handle(
      device, chrome_usb_connection_handle);
  return LIBUSB_SUCCESS;
}

void LibusbOverChromeUsb::LibusbClose(libusb_device_handle* device_handle) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  const RequestResult<chrome_usb::CloseDeviceResult> result =
      chrome_usb_api_bridge_->CloseDevice(
          device_handle->chrome_usb_connection_handle);
  if (!result.is_successful()) {
    // It's essential to not crash in this case, because this may happen during
    // shutdown process.
    GOOGLE_SMART_CARD_LOG_ERROR << "Failed to close USB device";
    return;
  }

  delete device_handle;
}

int LibusbOverChromeUsb::LibusbClaimInterface(
    libusb_device_handle* device_handle, int interface_number) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  const RequestResult<chrome_usb::ClaimInterfaceResult> result =
      chrome_usb_api_bridge_->ClaimInterface(
          device_handle->chrome_usb_connection_handle, interface_number);
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbClaimInterface request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  return LIBUSB_SUCCESS;
}

int LibusbOverChromeUsb::LibusbReleaseInterface(
    libusb_device_handle* device_handle, int interface_number) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  const RequestResult<chrome_usb::ReleaseInterfaceResult> result =
      chrome_usb_api_bridge_->ReleaseInterface(
          device_handle->chrome_usb_connection_handle, interface_number);
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbReleaseInterface request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  return LIBUSB_SUCCESS;
}

int LibusbOverChromeUsb::LibusbResetDevice(
    libusb_device_handle* device_handle) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  const RequestResult<chrome_usb::ResetDeviceResult> result =
      chrome_usb_api_bridge_->ResetDevice(
          device_handle->chrome_usb_connection_handle);
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbResetDevice request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  return LIBUSB_SUCCESS;
}

libusb_transfer* LibusbOverChromeUsb::LibusbAllocTransfer(
    int isochronous_packet_count) const {
  // Isochronous transfers are not supported.
  GOOGLE_SMART_CARD_CHECK(!isochronous_packet_count);

  libusb_transfer* const result = new libusb_transfer;
  std::memset(result, 0, sizeof(libusb_transfer));

  return result;
}

namespace {

bool CreateChromeUsbControlTransferInfo(
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char* data,
    uint16_t length,
    unsigned timeout,
    chrome_usb::ControlTransferInfo* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  result->direction =
      ((request_type & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) ?
      chrome_usb::Direction::kOut : chrome_usb::Direction::kIn;

  switch (request_type & kLibusbRequestRecipientMask) {
    case LIBUSB_RECIPIENT_DEVICE:
      result->recipient = chrome_usb::ControlTransferInfoRecipient::kDevice;
      break;
    case LIBUSB_RECIPIENT_INTERFACE:
      result->recipient = chrome_usb::ControlTransferInfoRecipient::kInterface;
      break;
    case LIBUSB_RECIPIENT_ENDPOINT:
      result->recipient = chrome_usb::ControlTransferInfoRecipient::kEndpoint;
      break;
    case LIBUSB_RECIPIENT_OTHER:
      result->recipient = chrome_usb::ControlTransferInfoRecipient::kOther;
      break;
    default:
      GOOGLE_SMART_CARD_NOTREACHED;
  }

  switch (request_type & kLibusbRequestTypeMask) {
    case LIBUSB_REQUEST_TYPE_STANDARD:
      result->request_type =
          chrome_usb::ControlTransferInfoRequestType::kStandard;
      break;
    case LIBUSB_REQUEST_TYPE_CLASS:
      result->request_type =
          chrome_usb::ControlTransferInfoRequestType::kClass;
      break;
    case LIBUSB_REQUEST_TYPE_VENDOR:
      result->request_type =
          chrome_usb::ControlTransferInfoRequestType::kVendor;
      break;
    case LIBUSB_REQUEST_TYPE_RESERVED:
      result->request_type =
          chrome_usb::ControlTransferInfoRequestType::kReserved;
      break;
    default:
      GOOGLE_SMART_CARD_NOTREACHED;
  }

  result->request = request;

  result->value = libusb_le16_to_cpu(value);

  result->index = libusb_le16_to_cpu(index);

  if (result->direction == chrome_usb::Direction::kIn)
    result->length = length;

  if (result->direction == chrome_usb::Direction::kOut) {
    GOOGLE_SMART_CARD_CHECK(data);
    result->data = MakeVarArrayBuffer(data, length);
  }

  result->timeout = timeout;

  return true;
}

bool CreateChromeUsbControlTransferInfo(
    libusb_transfer* transfer, chrome_usb::ControlTransferInfo* result) {
  GOOGLE_SMART_CARD_CHECK(transfer);
  GOOGLE_SMART_CARD_CHECK(transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL);
  GOOGLE_SMART_CARD_CHECK(result);

  //
  // Control-specific setup fields are kept in a special libusb_control_setup
  // structure placed in the beginning of the buffer; the real payload, that
  // will be sent to the Chrome USB API, is located further in the buffer (see
  // the convenience functions libusb_control_transfer_get_setup() and
  // libusb_control_transfer_get_data()).
  //
  // Note that the structure fields, according to the documentation, are
  // always stored in the little-endian byte order, so accesses to the
  // multi-byte fields (wValue, wIndex and wLength) must be carefully wrapped
  // through libusb_le16_to_cpu() macro.
  //

  if (transfer->length < 0 ||
      static_cast<size_t>(transfer->length) < LIBUSB_CONTROL_SETUP_SIZE) {
    return false;
  }

  const libusb_control_setup* const control_setup =
      libusb_control_transfer_get_setup(transfer);

  const uint16_t data_length = libusb_le16_to_cpu(control_setup->wLength);
  if (data_length != transfer->length - LIBUSB_CONTROL_SETUP_SIZE)
    return false;

  return CreateChromeUsbControlTransferInfo(
      control_setup->bmRequestType,
      control_setup->bRequest,
      libusb_le16_to_cpu(control_setup->wValue),
      libusb_le16_to_cpu(control_setup->wIndex),
      libusb_control_transfer_get_data(transfer),
      data_length,
      transfer->timeout,
      result);
}

void CreateChromeUsbGenericTransferInfo(
    unsigned char endpoint_address,
    unsigned char* data,
    int length,
    unsigned timeout,
    chrome_usb::GenericTransferInfo* result) {
  GOOGLE_SMART_CARD_CHECK(result);

  result->direction =
      ((endpoint_address & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) ?
      chrome_usb::Direction::kOut : chrome_usb::Direction::kIn;

  result->endpoint = endpoint_address;

  if (result->direction == chrome_usb::Direction::kIn)
    result->length = length;

  if (result->direction == chrome_usb::Direction::kOut) {
    GOOGLE_SMART_CARD_CHECK(data);
    result->data = MakeVarArrayBuffer(data, length);
  }

  result->timeout = timeout;
}

void CreateChromeUsbGenericTransferInfo(
    libusb_transfer* transfer, chrome_usb::GenericTransferInfo* result) {
  GOOGLE_SMART_CARD_CHECK(transfer);
  GOOGLE_SMART_CARD_CHECK(
      transfer->type == LIBUSB_TRANSFER_TYPE_BULK ||
      transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT);
  GOOGLE_SMART_CARD_CHECK(result);

  CreateChromeUsbGenericTransferInfo(
      transfer->endpoint,
      transfer->buffer,
      transfer->length,
      transfer->timeout,
      result);
}

}  // namespace

int LibusbOverChromeUsb::LibusbSubmitTransfer(libusb_transfer* transfer) {
  GOOGLE_SMART_CARD_CHECK(transfer);
  GOOGLE_SMART_CARD_CHECK(transfer->dev_handle);

  // Isochronous transfers are not supported.
  GOOGLE_SMART_CARD_CHECK(
      transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL ||
      transfer->type == LIBUSB_TRANSFER_TYPE_BULK ||
      transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT);

  if (transfer->flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET) {
    // Don't bother with this libusb feature (it's not even supported by it on
    // many platforms).
    return LIBUSB_ERROR_NOT_SUPPORTED;
  }

  libusb_context* const context = GetLibusbTransferContextChecked(transfer);

  AsyncRequest* async_request = nullptr;
  context->AddAsyncTransfer(transfer, &async_request);

  switch (transfer->type) {
    case LIBUSB_TRANSFER_TYPE_CONTROL: {
      chrome_usb::ControlTransferInfo transfer_info;
      if (!CreateChromeUsbControlTransferInfo(transfer, &transfer_info))
        return LIBUSB_ERROR_INVALID_PARAM;
      chrome_usb_api_bridge_->AsyncControlTransfer(
          transfer->dev_handle->chrome_usb_connection_handle,
          transfer_info,
          MakeAsyncTransferCallback(transfer),
          async_request);
      return LIBUSB_SUCCESS;
    }

    case LIBUSB_TRANSFER_TYPE_BULK: {
      chrome_usb::GenericTransferInfo transfer_info;
      CreateChromeUsbGenericTransferInfo(transfer, &transfer_info);
      chrome_usb_api_bridge_->AsyncBulkTransfer(
          transfer->dev_handle->chrome_usb_connection_handle,
          transfer_info,
          MakeAsyncTransferCallback(transfer),
          async_request);
      return LIBUSB_SUCCESS;
    }

    case LIBUSB_TRANSFER_TYPE_INTERRUPT: {
      chrome_usb::GenericTransferInfo transfer_info;
      CreateChromeUsbGenericTransferInfo(transfer, &transfer_info);
      chrome_usb_api_bridge_->AsyncInterruptTransfer(
          transfer->dev_handle->chrome_usb_connection_handle,
          transfer_info,
          MakeAsyncTransferCallback(transfer),
          async_request);
      return LIBUSB_SUCCESS;
    }

    default:
      GOOGLE_SMART_CARD_NOTREACHED;
  }
}

int LibusbOverChromeUsb::LibusbCancelTransfer(libusb_transfer* transfer) {
  GOOGLE_SMART_CARD_CHECK(transfer);

  libusb_context* const context = GetLibusbTransferContextChecked(transfer);
  GOOGLE_SMART_CARD_CHECK(context);
  return context->CancelAsyncTransfer(transfer) ?
      LIBUSB_SUCCESS : LIBUSB_ERROR_NOT_FOUND;
}

void LibusbOverChromeUsb::LibusbFreeTransfer(libusb_transfer* transfer) const {
  GOOGLE_SMART_CARD_CHECK(transfer);

  libusb_context* const context = GetLibusbTransferContext(transfer);
  if (context)
    context->RemoveAsyncTransfer(transfer);

  if (transfer->flags & LIBUSB_TRANSFER_FREE_BUFFER)
    ::free(transfer->buffer);
  delete transfer;
}

namespace {

libusb_transfer_status ProcessLibusbTransferResult(
    const chrome_usb::TransferResultInfo& transfer_result_info,
    bool is_short_not_ok,
    int length,
    unsigned char* data,
    int* actual_length) {
  if (!transfer_result_info.result_code)
    return LIBUSB_TRANSFER_ERROR;
  if (*transfer_result_info.result_code !=
      chrome_usb::kTransferResultInfoSuccessResultCode) {
    return LIBUSB_TRANSFER_ERROR;
  }

  // FIXME(emaxx): Looks like chrome.usb API returns timeout results as if they
  // were errors. So, in case of timeout, LIBUSB_TRANSFER_ERROR will be
  // returned to the consumers instead of returning LIBUSB_TRANSFER_TIMED_OUT.
  // This doesn't look like a huge problem, but still, from the sanity
  // prospective, this probably requires fixing.

  int actual_length_value;
  if (transfer_result_info.data) {
    actual_length_value = std::min(
        static_cast<int>(transfer_result_info.data->ByteLength()), length);
    if (actual_length_value) {
      const std::vector<uint8_t> data_vector = VarAs<std::vector<uint8_t>>(
          *transfer_result_info.data);
      std::copy_n(data_vector.begin(), actual_length_value, data);
    }
  } else {
    actual_length_value = length;
  }

  if (actual_length)
    *actual_length = actual_length_value;

  if (is_short_not_ok && actual_length_value < length)
    return LIBUSB_TRANSFER_ERROR;
  return LIBUSB_TRANSFER_COMPLETED;
}

int LibusbTransferStatusToLibusbErrorCode(
    libusb_transfer_status transfer_status) {
  switch (transfer_status) {
    case LIBUSB_TRANSFER_COMPLETED:
      return LIBUSB_SUCCESS;
    case LIBUSB_TRANSFER_TIMED_OUT:
      return LIBUSB_ERROR_TIMEOUT;
    default:
      return LIBUSB_ERROR_OTHER;
  }
}

}  // namespace

int LibusbOverChromeUsb::LibusbControlTransfer(
    libusb_device_handle* device_handle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char* data,
    uint16_t length,
    unsigned timeout) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  chrome_usb::ControlTransferInfo transfer_info;
  if (!CreateChromeUsbControlTransferInfo(
           request_type,
           request,
           value,
           index,
           data,
           length,
           timeout,
           &transfer_info)) {
    return LIBUSB_ERROR_INVALID_PARAM;
  }
  const RequestResult<chrome_usb::TransferResult> result =
      chrome_usb_api_bridge_->ControlTransfer(
          device_handle->chrome_usb_connection_handle, transfer_info);
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbControlTransfer request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  int actual_length;
  const int error_code = LibusbTransferStatusToLibusbErrorCode(
      ProcessLibusbTransferResult(
          result.payload().result_info, false, length, data, &actual_length));
  if (error_code == LIBUSB_SUCCESS)
    return actual_length;
  return error_code;
}

int LibusbOverChromeUsb::LibusbBulkTransfer(
    libusb_device_handle* device_handle,
    unsigned char endpoint_address,
    unsigned char* data,
    int length,
    int* actual_length,
    unsigned timeout) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  chrome_usb::GenericTransferInfo transfer_info;
  CreateChromeUsbGenericTransferInfo(
      endpoint_address, data, length, timeout, &transfer_info);
  const RequestResult<chrome_usb::TransferResult> result =
      chrome_usb_api_bridge_->BulkTransfer(
          device_handle->chrome_usb_connection_handle, transfer_info);
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbBulkTransfer request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  return LibusbTransferStatusToLibusbErrorCode(ProcessLibusbTransferResult(
      result.payload().result_info, false, length, data, actual_length));
}

int LibusbOverChromeUsb::LibusbInterruptTransfer(
    libusb_device_handle* device_handle,
    unsigned char endpoint_address,
    unsigned char* data,
    int length,
    int* actual_length,
    unsigned timeout) {
  GOOGLE_SMART_CARD_CHECK(device_handle);

  chrome_usb::GenericTransferInfo transfer_info;
  CreateChromeUsbGenericTransferInfo(
      endpoint_address, data, length, timeout, &transfer_info);
  const RequestResult<chrome_usb::TransferResult> result =
      chrome_usb_api_bridge_->InterruptTransfer(
          device_handle->chrome_usb_connection_handle, transfer_info);
  if (!result.is_successful()) {
    GOOGLE_SMART_CARD_LOG_WARNING <<
        "LibusbOverChromeUsb::LibusbInterruptTransfer request failed: " <<
        result.error_message();
    return LIBUSB_ERROR_OTHER;
  }
  return LibusbTransferStatusToLibusbErrorCode(ProcessLibusbTransferResult(
      result.payload().result_info, false, length, data, actual_length));
}

int LibusbOverChromeUsb::LibusbInit(libusb_context** context) {
  // If the default context was requested, do nothing (it's always existing and
  // initialized as long as this class object is alive).
  if (context)
    *context = new libusb_context;
  return LIBUSB_SUCCESS;
}

void LibusbOverChromeUsb::LibusbExit(libusb_context* context) {
  // If the default context deinitialization was requested, do nothing (it's
  // always kept initialized as long as this class object is alive).
  if (context)
    delete context;
}

int LibusbOverChromeUsb::LibusbHandleEvents(libusb_context* context) {
  return LibusbHandleEventsTimeout(context, kHandleEventsDefaultTimeoutSeconds);
}

int LibusbOverChromeUsb::LibusbHandleEventsTimeout(
    libusb_context* context, int timeout_seconds) {
  context = SubstituteDefaultContextIfNull(context);

  libusb_transfer* transfer;
  RequestResult<chrome_usb::TransferResult> request_result;
  if (context->WaitAndExtractCompletedAsyncTransfer(
          timeout_seconds, &transfer, &request_result)) {
    ProcessCompletedAsyncTransfer(transfer, std::move(request_result));
  }
  return LIBUSB_SUCCESS;
}

libusb_context* LibusbOverChromeUsb::SubstituteDefaultContextIfNull(
    libusb_context* context_or_nullptr) const {
  if (context_or_nullptr)
    return context_or_nullptr;
  return default_context_.get();
}

libusb_context* LibusbOverChromeUsb::GetLibusbTransferContext(
    const libusb_transfer* transfer) const {
  if (!transfer)
    return nullptr;
  libusb_device_handle* const device_handle = transfer->dev_handle;
  if (!device_handle)
    return nullptr;
  libusb_device* const device = device_handle->device;
  if (!device)
    return nullptr;
  return SubstituteDefaultContextIfNull(device->context());
}

libusb_context* LibusbOverChromeUsb::GetLibusbTransferContextChecked(
    const libusb_transfer* transfer) const {
  GOOGLE_SMART_CARD_CHECK(transfer);

  libusb_context* const result = GetLibusbTransferContext(transfer);
  GOOGLE_SMART_CARD_CHECK(result);
  return result;
}

chrome_usb::AsyncTransferCallback
LibusbOverChromeUsb::MakeAsyncTransferCallback(
    libusb_transfer* transfer) const {
  GOOGLE_SMART_CARD_CHECK(transfer);

  libusb_context* const context = GetLibusbTransferContextChecked(transfer);
  GOOGLE_SMART_CARD_CHECK(context);

  return [transfer, context](
      RequestResult<chrome_usb::TransferResult> request_result) {
    context->AddCompletedAsyncTransfer(transfer, std::move(request_result));
  };
}

void LibusbOverChromeUsb::ProcessCompletedAsyncTransfer(
    libusb_transfer* transfer,
    RequestResult<chrome_usb::TransferResult> request_result) const {
  GOOGLE_SMART_CARD_CHECK(transfer);

  if (request_result.is_successful()) {
    //
    // Note that the control transfers have a special libusb_control_setup
    // structure placed in the beginning of the buffer (it contains some
    // control-specific setup; see also |CreateChromeUsbControlTransferInfo|
    // function implementation for more details). So, as chrome.usb API doesn't
    // operate with these setup structures, we must place the received response
    // data under some offset (using a helper function
    // libusb_control_transfer_get_data()).
    //
    unsigned char* const buffer =
        transfer->type != LIBUSB_TRANSFER_TYPE_CONTROL
            ? transfer->buffer : libusb_control_transfer_get_data(transfer);

    transfer->status = ProcessLibusbTransferResult(
        request_result.payload().result_info,
        (transfer->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) != 0,
        transfer->length,
        buffer,
        &transfer->actual_length);
  } else if (request_result.status() == RequestResultStatus::kCanceled) {
    transfer->status = LIBUSB_TRANSFER_CANCELLED;
  } else {
    transfer->status = LIBUSB_TRANSFER_ERROR;
  }

  transfer->callback(transfer);

  if (transfer->flags & LIBUSB_TRANSFER_FREE_TRANSFER)
    LibusbFreeTransfer(transfer);
}

}  // namespace google_smart_card