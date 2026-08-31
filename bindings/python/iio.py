#!/usr/bin/env python
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
SPDX-License-Identifier: LGPL-2.1-or-later

Copyright (C) 2014 Analog Devices, Inc.
Author: Paul Cercueil <paul.cercueil@analog.com>
"""

# Imports from package ctypes are not grouped
# pylint: disable=ungrouped-imports
#
# The way the methods and classes are used, violate this, but there
#   isn't a good way to do things otherwise
# pylint: disable=protected-access
from ctypes import (
    Structure,
    c_char_p,
    c_uint,
    c_int,
    c_long,
    c_longlong,
    c_size_t,
    c_ssize_t,
    c_char,
    c_void_p,
    c_bool,
    create_string_buffer,
    c_double,
    cast,
    POINTER as _POINTER,
    CDLL as _cdll,
    memmove as _memmove,
    byref as _byref,
    sizeof as _sizeof,
)
from ctypes.util import find_library
from enum import Enum
from errno import ENOSYS as _ENOSYS
from os import strerror as _strerror
from platform import system as _system
import abc
import struct as _struct

if "Windows" in _system():
    from ctypes import get_last_error
else:
    from ctypes import get_errno
# pylint: enable=ungrouped-imports

# ctypes requires the errcheck to take three arguments, but we don't use them
# pylint: disable=unused-argument


def _check_null(result, func, arguments):
    if result:
        return result
    err = get_last_error() if "Windows" in _system() else get_errno()
    raise OSError(err, _strerror(err))


def _check_negative(result, func, arguments):
    if result >= 0:
        return result
    raise OSError(-result, _strerror(-result))


# pylint: enable=unused-argument

# Python 2 and Python 3 compatible _isstring function.
# pylint: disable=basestring-builtin
def _isstring(argument):
    try:
        # attempt to evaluate basestring (Python 2)
        return isinstance(argument, basestring)
    except NameError:
        # No basestring --> Python 3
        return isinstance(argument, str)


# pylint: enable=basestring-builtin


class _ScanContext(Structure):
    pass


class _ContextInfo(Structure):
    pass


class _Context(Structure):
    pass


class _Device(Structure):
    pass


class _Channel(Structure):
    pass


class _Buffer(Structure):
    pass


class DataFormat(Structure):
    """Represents the data format of an IIO channel."""

    _fields_ = [
        ("length", c_uint),
        ("bits", c_uint),
        ("shift", c_uint),
        ("is_signed", c_bool),
        ("is_fully_defined", c_bool),
        ("is_be", c_bool),
        ("with_scale", c_bool),
        ("scale", c_double),
        ("repeat", c_uint),
    ]


class ChannelModifier(Enum):
    """Contains the modifier type of an IIO channel."""

    IIO_NO_MOD = 0
    IIO_MOD_X = 1
    IIO_MOD_Y = 2
    IIO_MOD_Z = 3
    IIO_MOD_X_AND_Y = 4
    IIO_MOD_X_AND_Z = 5
    IIO_MOD_Y_AND_Z = 6
    IIO_MOD_X_AND_Y_AND_Z = 7
    IIO_MOD_X_OR_Y = 8
    IIO_MOD_X_OR_Z = 9
    IIO_MOD_Y_OR_Z = 10
    IIO_MOD_X_OR_Y_OR_Z = 11
    IIO_MOD_LIGHT_BOTH = 12
    IIO_MOD_LIGHT_IR = 13
    IIO_MOD_ROOT_SUM_SQUARED_X_Y = 14
    IIO_MOD_SUM_SQUARED_X_Y_Z = 15
    IIO_MOD_LIGHT_CLEAR = 16
    IIO_MOD_LIGHT_RED = 17
    IIO_MOD_LIGHT_GREEN = 18
    IIO_MOD_LIGHT_BLUE = 19
    IIO_MOD_QUATERNION = 20
    IIO_MOD_TEMP_AMBIENT = 21
    IIO_MOD_TEMP_OBJECT = 22
    IIO_MOD_NORTH_MAGN = 23
    IIO_MOD_NORTH_TRUE = 24
    IIO_MOD_NORTH_MAGN_TILT_COMP = 25
    IIO_MOD_NORTH_TRUE_TILT_COMP = 26
    IIO_MOD_RUNNING = 27
    IIO_MOD_JOGGING = 28
    IIO_MOD_WALKING = 29
    IIO_MOD_STILL = 30
    IIO_MOD_ROOT_SUM_SQUARED_X_Y_Z = 31
    IIO_MOD_I = 32
    IIO_MOD_Q = 33
    IIO_MOD_CO2 = 34
    IIO_MOD_VOC = 35
    IIO_MOD_LIGHT_UV = 36
    IIO_MOD_LIGHT_DUV = 37
    IIO_MOD_PM1 = 38
    IIO_MOD_PM2P5 = 39
    IIO_MOD_PM4 = 40
    IIO_MOD_PM10 = 41
    IIO_MOD_ETHANOL = 42
    IIO_MOD_H2 = 43
    IIO_MOD_O2 = 44


class ChannelType(Enum):
    """Contains the type of an IIO channel."""

    IIO_VOLTAGE = 0
    IIO_CURRENT = 1
    IIO_POWER = 2
    IIO_ACCEL = 3
    IIO_ANGL_VEL = 4
    IIO_MAGN = 5
    IIO_LIGHT = 6
    IIO_INTENSITY = 7
    IIO_PROXIMITY = 8
    IIO_TEMP = 9
    IIO_INCLI = 10
    IIO_ROT = 11
    IIO_ANGL = 12
    IIO_TIMESTAMP = 13
    IIO_CAPACITANCE = 14
    IIO_ALTVOLTAGE = 15
    IIO_CCT = 16
    IIO_PRESSURE = 17
    IIO_HUMIDITYRELATIVE = 18
    IIO_ACTIVITY = 19
    IIO_STEPS = 20
    IIO_ENERGY = 21
    IIO_DISTANCE = 22
    IIO_VELOCITY = 23
    IIO_CONCENTRATION = 24
    IIO_RESISTANCE = 25
    IIO_PH = 26
    IIO_UVINDEX = 27
    IIO_ELECTRICALCONDUCTIVITY = 28
    IIO_COUNT = 29
    IIO_INDEX = 30
    IIO_GRAVITY = 31
    IIO_POSITIONRELATIVE = 32
    IIO_PHASE = 33
    IIO_MASSCONCENTRATION = 34
    IIO_CHAN_TYPE_UNKNOWN = 0x7FFFFFFF


# pylint: disable=invalid-name
_ScanContextPtr = _POINTER(_ScanContext)
_ContextInfoPtr = _POINTER(_ContextInfo)
_ContextPtr = _POINTER(_Context)
_DevicePtr = _POINTER(_Device)
_ChannelPtr = _POINTER(_Channel)
_BufferPtr = _POINTER(_Buffer)
_DataFormatPtr = _POINTER(DataFormat)

if "Windows" in _system():
    _iiolib = "libiio.dll"
else:
    # Non-windows, possibly Posix system
    _iiolib = "iio"

_lib = _cdll(find_library(_iiolib), use_errno=True, use_last_error=True)

_get_backends_count = _lib.iio_get_backends_count
_get_backends_count.restype = c_uint

_get_backend = _lib.iio_get_backend
_get_backend.argtypes = (c_uint,)
_get_backend.restype = c_char_p
_get_backend.errcheck = _check_null

_create_scan_context = _lib.iio_create_scan_context
_create_scan_context.argtypes = (c_char_p, c_uint)
_create_scan_context.restype = _ScanContextPtr
_create_scan_context.errcheck = _check_null

_destroy_scan_context = _lib.iio_scan_context_destroy
_destroy_scan_context.argtypes = (_ScanContextPtr,)

_iio_has_backend = _lib.iio_has_backend
_iio_has_backend.argtypes = (c_char_p,)
_iio_has_backend.restype = c_bool

_iio_strerror = _lib.iio_strerror
_iio_strerror.argtypes = (c_int, c_char_p, c_uint)
_iio_strerror.restype = None

_get_context_info_list = _lib.iio_scan_context_get_info_list
_get_context_info_list.argtypes = (_ScanContextPtr, _POINTER(_POINTER(_ContextInfoPtr)))
_get_context_info_list.restype = c_ssize_t
_get_context_info_list.errcheck = _check_negative

_context_info_list_free = _lib.iio_context_info_list_free
_context_info_list_free.argtypes = (_POINTER(_ContextInfoPtr),)

_context_info_get_description = _lib.iio_context_info_get_description
_context_info_get_description.argtypes = (_ContextInfoPtr,)
_context_info_get_description.restype = c_char_p

_context_info_get_uri = _lib.iio_context_info_get_uri
_context_info_get_uri.argtypes = (_ContextInfoPtr,)
_context_info_get_uri.restype = c_char_p

_new_local = _lib.iio_create_local_context
_new_local.restype = _ContextPtr
_new_local.errcheck = _check_null

_new_xml = _lib.iio_create_xml_context
_new_xml.restype = _ContextPtr
_new_xml.argtypes = (c_char_p,)
_new_xml.errcheck = _check_null

_new_network = _lib.iio_create_network_context
_new_network.restype = _ContextPtr
_new_network.argtypes = (c_char_p,)
_new_network.errcheck = _check_null

_new_default = _lib.iio_create_default_context
_new_default.restype = _ContextPtr
_new_default.errcheck = _check_null

_new_uri = _lib.iio_create_context_from_uri
_new_uri.restype = _ContextPtr
_new_uri.errcheck = _check_null

_destroy = _lib.iio_context_destroy
_destroy.argtypes = (_ContextPtr,)

_get_name = _lib.iio_context_get_name
_get_name.restype = c_char_p
_get_name.argtypes = (_ContextPtr,)
_get_name.errcheck = _check_null

_get_description = _lib.iio_context_get_description
_get_description.restype = c_char_p
_get_description.argtypes = (_ContextPtr,)

_get_xml = _lib.iio_context_get_xml
_get_xml.restype = c_char_p
_get_xml.argtypes = (_ContextPtr,)

_get_library_version = _lib.iio_library_get_version
_get_library_version.argtypes = (
    _POINTER(c_uint),
    _POINTER(c_uint),
    c_char_p,
)

_get_version = _lib.iio_context_get_version
_get_version.restype = c_int
_get_version.argtypes = (
    _ContextPtr,
    _POINTER(c_uint),
    _POINTER(c_uint),
    c_char_p,
)
_get_version.errcheck = _check_negative

_get_attrs_count = _lib.iio_context_get_attrs_count
_get_attrs_count.restype = c_uint
_get_attrs_count.argtypes = (_ContextPtr,)

_get_attr = _lib.iio_context_get_attr
_get_attr.restype = c_int
_get_attr.argtypes = (_ContextPtr, c_uint, _POINTER(c_char_p), _POINTER(c_char_p))
_get_attr.errcheck = _check_negative

_devices_count = _lib.iio_context_get_devices_count
_devices_count.restype = c_uint
_devices_count.argtypes = (_ContextPtr,)

_get_device = _lib.iio_context_get_device
_get_device.restype = _DevicePtr
_get_device.argtypes = (_ContextPtr, c_uint)
_get_device.errcheck = _check_null

_find_device = _lib.iio_context_find_device
_find_device.restype = _DevicePtr
_find_device.argtypes = (_ContextPtr, c_char_p)

_set_timeout = _lib.iio_context_set_timeout
_set_timeout.restype = c_int
_set_timeout.argtypes = (
    _ContextPtr,
    c_uint,
)
_set_timeout.errcheck = _check_negative

_clone = _lib.iio_context_clone
_clone.restype = _ContextPtr
_clone.argtypes = (_ContextPtr,)
_clone.errcheck = _check_null

_d_get_id = _lib.iio_device_get_id
_d_get_id.restype = c_char_p
_d_get_id.argtypes = (_DevicePtr,)
_d_get_id.errcheck = _check_null

_d_get_name = _lib.iio_device_get_name
_d_get_name.restype = c_char_p
_d_get_name.argtypes = (_DevicePtr,)

_d_get_label = _lib.iio_device_get_label
_d_get_label.restype = c_char_p
_d_get_label.argtypes = (_DevicePtr,)

_d_attr_count = _lib.iio_device_get_attrs_count
_d_attr_count.restype = c_uint
_d_attr_count.argtypes = (_DevicePtr,)

_d_get_attr = _lib.iio_device_get_attr
_d_get_attr.restype = c_char_p
_d_get_attr.argtypes = (_DevicePtr, c_uint)
_d_get_attr.errcheck = _check_null

_d_read_attr = _lib.iio_device_attr_read
_d_read_attr.restype = c_ssize_t
_d_read_attr.argtypes = (_DevicePtr, c_char_p, c_char_p, c_size_t)
_d_read_attr.errcheck = _check_negative

_d_write_attr = _lib.iio_device_attr_write
_d_write_attr.restype = c_ssize_t
_d_write_attr.argtypes = (_DevicePtr, c_char_p, c_char_p)
_d_write_attr.errcheck = _check_negative

_d_debug_attr_count = _lib.iio_device_get_debug_attrs_count
_d_debug_attr_count.restype = c_uint
_d_debug_attr_count.argtypes = (_DevicePtr,)

_d_get_debug_attr = _lib.iio_device_get_debug_attr
_d_get_debug_attr.restype = c_char_p
_d_get_debug_attr.argtypes = (_DevicePtr, c_uint)
_d_get_debug_attr.errcheck = _check_null

_d_read_debug_attr = _lib.iio_device_debug_attr_read
_d_read_debug_attr.restype = c_ssize_t
_d_read_debug_attr.argtypes = (_DevicePtr, c_char_p, c_char_p, c_size_t)
_d_read_debug_attr.errcheck = _check_negative

_d_write_debug_attr = _lib.iio_device_debug_attr_write
_d_write_debug_attr.restype = c_ssize_t
_d_write_debug_attr.argtypes = (_DevicePtr, c_char_p, c_char_p)
_d_write_debug_attr.errcheck = _check_negative

_d_buffer_attr_count = _lib.iio_device_get_buffer_attrs_count
_d_buffer_attr_count.restype = c_uint
_d_buffer_attr_count.argtypes = (_DevicePtr,)

_d_get_buffer_attr = _lib.iio_device_get_buffer_attr
_d_get_buffer_attr.restype = c_char_p
_d_get_buffer_attr.argtypes = (_DevicePtr, c_uint)
_d_get_buffer_attr.errcheck = _check_null

_d_read_buffer_attr = _lib.iio_device_buffer_attr_read
_d_read_buffer_attr.restype = c_ssize_t
_d_read_buffer_attr.argtypes = (_DevicePtr, c_char_p, c_char_p, c_size_t)
_d_read_buffer_attr.errcheck = _check_negative

_d_write_buffer_attr = _lib.iio_device_buffer_attr_write
_d_write_buffer_attr.restype = c_ssize_t
_d_write_buffer_attr.argtypes = (_DevicePtr, c_char_p, c_char_p)
_d_write_buffer_attr.errcheck = _check_negative

_d_get_context = _lib.iio_device_get_context
_d_get_context.restype = _ContextPtr
_d_get_context.argtypes = (_DevicePtr,)

_d_find_channel = _lib.iio_device_find_channel
_d_find_channel.restype = _ChannelPtr
_d_find_channel.argtypes = (_DevicePtr, c_char_p, c_bool)

_d_reg_write = _lib.iio_device_reg_write
_d_reg_write.restype = c_int
_d_reg_write.argtypes = (_DevicePtr, c_uint, c_uint)
_d_reg_write.errcheck = _check_negative

_d_reg_read = _lib.iio_device_reg_read
_d_reg_read.restype = c_int
_d_reg_read.argtypes = (_DevicePtr, c_uint, _POINTER(c_uint))
_d_reg_read.errcheck = _check_negative

_channels_count = _lib.iio_device_get_channels_count
_channels_count.restype = c_uint
_channels_count.argtypes = (_DevicePtr,)

_get_channel = _lib.iio_device_get_channel
_get_channel.restype = _ChannelPtr
_get_channel.argtypes = (_DevicePtr, c_uint)
_get_channel.errcheck = _check_null

_get_sample_size = _lib.iio_device_get_sample_size
_get_sample_size.restype = c_int
_get_sample_size.argtypes = (_DevicePtr,)
_get_sample_size.errcheck = _check_negative

_d_is_trigger = _lib.iio_device_is_trigger
_d_is_trigger.restype = c_bool
_d_is_trigger.argtypes = (_DevicePtr,)

_d_get_trigger = _lib.iio_device_get_trigger
_d_get_trigger.restype = c_int
_d_get_trigger.argtypes = (
    _DevicePtr,
    _POINTER(_DevicePtr),
)
_d_get_trigger.errcheck = _check_negative

_d_set_trigger = _lib.iio_device_set_trigger
_d_set_trigger.restype = c_int
_d_set_trigger.argtypes = (
    _DevicePtr,
    _DevicePtr,
)
_d_set_trigger.errcheck = _check_negative

_d_set_buffers_count = _lib.iio_device_set_kernel_buffers_count
_d_set_buffers_count.restype = c_int
_d_set_buffers_count.argtypes = (_DevicePtr, c_uint)
_d_set_buffers_count.errcheck = _check_negative

_d_get_buffers_count = _lib.iio_device_get_kernel_buffers_count
_d_get_buffers_count.restype = c_uint
_d_get_buffers_count.argtypes = (_DevicePtr,)

_c_get_id = _lib.iio_channel_get_id
_c_get_id.restype = c_char_p
_c_get_id.argtypes = (_ChannelPtr,)
_c_get_id.errcheck = _check_null

_c_get_name = _lib.iio_channel_get_name
_c_get_name.restype = c_char_p
_c_get_name.argtypes = (_ChannelPtr,)

_c_is_output = _lib.iio_channel_is_output
_c_is_output.restype = c_bool
_c_is_output.argtypes = (_ChannelPtr,)

_c_is_scan_element = _lib.iio_channel_is_scan_element
_c_is_scan_element.restype = c_bool
_c_is_scan_element.argtypes = (_ChannelPtr,)

_c_attr_count = _lib.iio_channel_get_attrs_count
_c_attr_count.restype = c_uint
_c_attr_count.argtypes = (_ChannelPtr,)

_c_get_attr = _lib.iio_channel_get_attr
_c_get_attr.restype = c_char_p
_c_get_attr.argtypes = (_ChannelPtr, c_uint)
_c_get_attr.errcheck = _check_null

_c_get_filename = _lib.iio_channel_attr_get_filename
_c_get_filename.restype = c_char_p
_c_get_filename.argtypes = (
    _ChannelPtr,
    c_char_p,
)
_c_get_filename.errcheck = _check_null

_c_read_attr = _lib.iio_channel_attr_read
_c_read_attr.restype = c_ssize_t
_c_read_attr.argtypes = (_ChannelPtr, c_char_p, c_char_p, c_size_t)
_c_read_attr.errcheck = _check_negative

_c_write_attr = _lib.iio_channel_attr_write
_c_write_attr.restype = c_ssize_t
_c_write_attr.argtypes = (_ChannelPtr, c_char_p, c_char_p)
_c_write_attr.errcheck = _check_negative

_c_enable = _lib.iio_channel_enable
_c_enable.argtypes = (_ChannelPtr,)

_c_disable = _lib.iio_channel_disable
_c_disable.argtypes = (_ChannelPtr,)

_c_is_enabled = _lib.iio_channel_is_enabled
_c_is_enabled.restype = c_bool
_c_is_enabled.argtypes = (_ChannelPtr,)

_c_read = _lib.iio_channel_read
_c_read.restype = c_ssize_t
_c_read.argtypes = (
    _ChannelPtr,
    _BufferPtr,
    c_void_p,
    c_size_t,
)

_c_read_raw = _lib.iio_channel_read_raw
_c_read_raw.restype = c_ssize_t
_c_read_raw.argtypes = (
    _ChannelPtr,
    _BufferPtr,
    c_void_p,
    c_size_t,
)

_c_write = _lib.iio_channel_write
_c_write.restype = c_ssize_t
_c_write.argtypes = (
    _ChannelPtr,
    _BufferPtr,
    c_void_p,
    c_size_t,
)

_c_write_raw = _lib.iio_channel_write_raw
_c_write_raw.restype = c_ssize_t
_c_write_raw.argtypes = (
    _ChannelPtr,
    _BufferPtr,
    c_void_p,
    c_size_t,
)

_channel_get_device = _lib.iio_channel_get_device
_channel_get_device.restype = _DevicePtr
_channel_get_device.argtypes = (_ChannelPtr,)

_channel_get_index = _lib.iio_channel_get_index
_channel_get_index.restype = c_long
_channel_get_index.argtypes = (_ChannelPtr,)

_channel_get_data_format = _lib.iio_channel_get_data_format
_channel_get_data_format.restype = _DataFormatPtr
_channel_get_data_format.argtypes = (_ChannelPtr,)

_channel_get_modifier = _lib.iio_channel_get_modifier
_channel_get_modifier.restype = c_int
_channel_get_modifier.argtypes = (_ChannelPtr,)

_channel_get_type = _lib.iio_channel_get_type
_channel_get_type.restype = c_int
_channel_get_type.argtypes = (_ChannelPtr,)

_create_buffer = _lib.iio_device_create_buffer
_create_buffer.restype = _BufferPtr
_create_buffer.argtypes = (
    _DevicePtr,
    c_size_t,
    c_bool,
)
_create_buffer.errcheck = _check_null

_create_buffer_with_metadata = _lib.iio_device_create_buffer_with_metadata
_create_buffer_with_metadata.restype = _BufferPtr
_create_buffer_with_metadata.argtypes = (
    _DevicePtr,
    c_size_t,
    c_void_p,
    c_size_t,
)
_create_buffer_with_metadata.errcheck = _check_null

_buffer_destroy = _lib.iio_buffer_destroy
_buffer_destroy.argtypes = (_BufferPtr,)

_buffer_refill = _lib.iio_buffer_refill
_buffer_refill.restype = c_ssize_t
_buffer_refill.argtypes = (_BufferPtr,)
_buffer_refill.errcheck = _check_negative

_buffer_refill_with_metadata = _lib.iio_buffer_refill_with_metadata
_buffer_refill_with_metadata.restype = c_ssize_t
_buffer_refill_with_metadata.argtypes = (
    _BufferPtr,
    c_void_p,
    c_size_t,
    _POINTER(c_size_t),
)
_buffer_refill_with_metadata.errcheck = _check_negative

_buffer_set_metadata_batch_size = _lib.iio_buffer_set_metadata_batch_size
_buffer_set_metadata_batch_size.restype = c_int
_buffer_set_metadata_batch_size.argtypes = (_BufferPtr, c_uint)
_buffer_set_metadata_batch_size.errcheck = _check_negative

try:
    _buffer_set_metadata_read_prequeue_async = (
        _lib.iio_buffer_set_metadata_read_prequeue_async
    )
except AttributeError:
    _buffer_set_metadata_read_prequeue_async = None
else:
    _buffer_set_metadata_read_prequeue_async.restype = c_int
    _buffer_set_metadata_read_prequeue_async.argtypes = (
        _BufferPtr,
        c_uint,
        c_size_t,
    )
    _buffer_set_metadata_read_prequeue_async.errcheck = _check_negative

_buffer_get_metadata_status = _lib.iio_buffer_get_metadata_status
_buffer_get_metadata_status.restype = c_ssize_t
_buffer_get_metadata_status.argtypes = (_BufferPtr, c_void_p, c_size_t)
_buffer_get_metadata_status.errcheck = _check_negative

_buffer_push_partial = _lib.iio_buffer_push_partial
_buffer_push_partial.restype = c_ssize_t
_buffer_push_partial.argtypes = (
    _BufferPtr,
    c_uint,
)
_buffer_push_partial.errcheck = _check_negative

_buffer_start = _lib.iio_buffer_start
_buffer_start.restype = c_void_p
_buffer_start.argtypes = (_BufferPtr,)

_buffer_end = _lib.iio_buffer_end
_buffer_end.restype = c_void_p
_buffer_end.argtypes = (_BufferPtr,)

_buffer_cancel = _lib.iio_buffer_cancel
_buffer_cancel.restype = c_void_p
_buffer_cancel.argtypes = (_BufferPtr,)

_buffer_get_device = _lib.iio_buffer_get_device
_buffer_get_device.restype = _DevicePtr
_buffer_get_device.argtypes = (_BufferPtr,)

_buffer_get_poll_fd = _lib.iio_buffer_get_poll_fd
_buffer_get_poll_fd.restype = c_int
_buffer_get_poll_fd.argtypes = (_BufferPtr,)

_buffer_step = _lib.iio_buffer_step
_buffer_step.restype = c_longlong
_buffer_step.argtypes = (_BufferPtr,)

_buffer_set_blocking_mode = _lib.iio_buffer_set_blocking_mode
_buffer_set_blocking_mode.restype = c_uint
_buffer_set_blocking_mode.argtypes = (_BufferPtr, c_bool)


# pylint: enable=invalid-name


def _get_lib_version():
    major = c_uint()
    minor = c_uint()
    buf = create_string_buffer(8)
    _get_library_version(_byref(major), _byref(minor), buf)
    return (major.value, minor.value, buf.value.decode("ascii"))


def _has_backend(backend):
    b_backend = backend.encode("utf-8")
    return _iio_has_backend(b_backend)


def iio_strerror(err, buf, length):
    """Get a string description of an error code."""
    b_buf = buf.encode("utf-8")
    _iio_strerror(err, b_buf, length)


version = _get_lib_version()
backends = [_get_backend(b).decode("ascii") for b in range(0, _get_backends_count())]


class _Attr(object):
    def __init__(self, name, filename=None):
        self._name = name
        self._name_ascii = name.encode("ascii")
        self._filename = name if filename is None else filename

    def __str__(self):
        return self._name

    @abc.abstractmethod
    def _read(self):
        pass

    @abc.abstractmethod
    def _write(self, value):
        pass

    name = property(
        lambda self: self._name, None, None, "The name of this attribute.\n\ttype=str"
    )
    filename = property(
        lambda self: self._filename,
        None,
        None,
        "The filename in sysfs to which this attribute is bound.\n\ttype=str",
    )
    value = property(
        lambda self: self._read(),
        lambda self, x: self._write(x),
        None,
        "Current value of this attribute.\n\ttype=str",
    )


class ChannelAttr(_Attr):
    """Represents an attribute of a channel."""

    def __init__(self, channel, name):
        """
        Initialize a new instance of the ChannelAttr class.

        :param channel: type=iio.Channel
            A valid instance of the iio.Channel class.
        :param name: type=str
            The channel attribute's name

        returns: type=iio.ChannelAttr
            A new instance of this class
        """
        super(ChannelAttr, self).__init__(
            name, _c_get_filename(channel, name.encode("ascii")).decode("ascii")
        )
        self._channel = channel

    def _read(self):
        buf = create_string_buffer(1024)
        _c_read_attr(self._channel, self._name_ascii, buf, len(buf))
        return buf.value.decode("ascii")

    def _write(self, value):
        _c_write_attr(self._channel, self._name_ascii, value.encode("ascii"))


class DeviceAttr(_Attr):
    """Represents an attribute of an IIO device."""

    def __init__(self, device, name):
        """
        Initialize a new instance of the DeviceAttr class.

        :param device: type=iio.Device
            A valid instance of the iio.Device class.
        :param name: type=str
            The device attribute's name

        returns: type=iio.DeviceAttr
            A new instance of this class
        """
        super(DeviceAttr, self).__init__(name)
        self._device = device

    def _read(self):
        buf = create_string_buffer(1024)
        _d_read_attr(self._device, self._name_ascii, buf, len(buf))
        return buf.value.decode("ascii")

    def _write(self, value):
        _d_write_attr(self._device, self._name_ascii, value.encode("ascii"))


class DeviceDebugAttr(DeviceAttr):
    """Represents a debug attribute of an IIO device."""

    def __init__(self, device, name):
        """
        Initialize a new instance of the DeviceDebugAttr class.

        :param device: type=iio.Device
             A valid instance of the iio.Device class.
        :param name: type=str
             The device debug attribute's name

        returns: type=iio.DeviceDebugAttr
            A new instance of this class
        """
        super(DeviceDebugAttr, self).__init__(device, name)

    def _read(self):
        buf = create_string_buffer(1024)
        _d_read_debug_attr(self._device, self._name_ascii, buf, len(buf))
        return buf.value.decode("ascii")

    def _write(self, value):
        _d_write_debug_attr(self._device, self._name_ascii, value.encode("ascii"))


class DeviceBufferAttr(DeviceAttr):
    """Represents a buffer attribute of an IIO device."""

    def __init__(self, device, name):
        """
        Initialize a new instance of the DeviceBufferAttr class.

        :param device: type=iio.Device
            A valid instance of the iio.Device class.
        :param name: type=str
            The device buffer attribute's name

        returns: type=iio.DeviceBufferAttr
            A new instance of this class
        """
        super(DeviceBufferAttr, self).__init__(device, name)

    def _read(self):
        buf = create_string_buffer(1024)
        _d_read_buffer_attr(self._device, self._name_ascii, buf, len(buf))
        return buf.value.decode("ascii")

    def _write(self, value):
        _d_write_buffer_attr(self._device, self._name_ascii, value.encode("ascii"))


class Channel(object):
    """Represents a channel of an IIO device."""

    def __init__(self, dev, _channel):
        """
        Initialize a new instance of the Channel class.

        :param _channel: type=_ChannelPtr
            Pointer to an IIO Channel.

        returns: type=iio.Channel
            An new instance of this class
        """
        self._channel = _channel
        self._dev = dev
        self._attrs = {
            name: ChannelAttr(_channel, name)
            for name in [
                _c_get_attr(_channel, x).decode("ascii")
                for x in range(0, _c_attr_count(_channel))
            ]
        }
        self._id = _c_get_id(self._channel).decode("ascii")

        name_raw = _c_get_name(self._channel)
        self._name = name_raw.decode("ascii") if name_raw is not None else None
        self._output = _c_is_output(self._channel)
        self._scan_element = _c_is_scan_element(self._channel)

    def read(self, buf, raw=False):
        """
        Extract the samples corresponding to this channel from the given iio.Buffer object.

        :param buf: type=iio.Buffer
            A valid instance of the iio.Buffer class
        :param raw: type=bool
            If set to True, the samples are not converted from their
            native format to their host format

        returns: type=bytearray
            An array containing the samples for this channel
        """
        array = bytearray(buf._length)
        mytype = c_char * len(array)
        c_array = mytype.from_buffer(array)
        if raw:
            length = _c_read_raw(self._channel, buf._buffer, c_array, len(array))
        else:
            length = _c_read(self._channel, buf._buffer, c_array, len(array))
        return array[:length]

    def write(self, buf, array, raw=False):
        """
        Write the specified array of samples corresponding to this channel into the given iio.Buffer object.

        :param buf: type=iio.Buffer
            A valid instance of the iio.Buffer class
        :param array: type=bytearray
            The array containing the samples to copy
        :param raw: type=bool
            If set to True, the samples are not converted from their
            host format to their native format

        returns: type=int
            The number of bytes written
        """
        mytype = c_char * len(array)
        c_array = mytype.from_buffer(array)
        if raw:
            return _c_write_raw(self._channel, buf._buffer, c_array, len(array))
        return _c_write(self._channel, buf._buffer, c_array, len(array))

    id = property(
        lambda self: self._id,
        None,
        None,
        "An identifier of this channel.\n\tNote that it is possible that two channels have the same ID, if one is an input channel and the other is an output channel.\n\ttype=str",
    )
    name = property(
        lambda self: self._name, None, None, "The name of this channel.\n\ttype=str"
    )
    attrs = property(
        lambda self: self._attrs,
        None,
        None,
        "List of attributes for this channel.\n\ttype=dict of iio.ChannelAttr",
    )
    output = property(
        lambda self: self._output,
        None,
        None,
        "Contains True if the channel is an output channel, False otherwise.\n\ttype=bool",
    )
    scan_element = property(
        lambda self: self._scan_element,
        None,
        None,
        "Contains True if the channel is a scan element, False otherwise.\n\tIf a channel is a scan element, then it is possible to enable it and use it for I/O operations.\n\ttype=bool",
    )
    enabled = property(
        lambda self: _c_is_enabled(self._channel),
        lambda self, x: _c_enable(self._channel) if x else _c_disable(self._channel),
        None,
        "Configured state of the channel\n\ttype=bool",
    )

    @property
    def device(self):
        """
        Corresponding device for the channel.
        type: iio.Device
        """
        return self._dev

    @property
    def index(self):
        """Index for the channel."""
        return _channel_get_index(self._channel)

    @property
    def data_format(self):
        """
        Channel data format.
        type: iio.DataFormat
        """
        return _channel_get_data_format(self._channel).contents

    @property
    def modifier(self):
        """
        Channel modifier.
        type: iio.ChannelModifier(Enum)
        """
        return ChannelModifier(_channel_get_modifier(self._channel))

    @property
    def type(self):
        """
        Type for the channel.
        type: iio.ChannelType(Enum)
        """
        return ChannelType(_channel_get_type(self._channel))


class Buffer(object):
    """The class used for all I/O operations."""

    def __init__(self, device, samples_count, cyclic=False):
        """
        Initialize a new instance of the Buffer class.

        :param device: type=iio.Device
            The iio.Device object that represents the device where the I/O
            operations will be performed
        :param samples_count: type=int
            The size of the buffer, in samples
        :param circular: type=bool
            If set to True, the buffer is circular

        returns: type=iio.Buffer
            An new instance of this class
        """
        try:
            self._buffer = _create_buffer(device._device, samples_count, cyclic)
        except Exception:
            self._buffer = None
            raise
        self._length = samples_count * device.sample_size
        self._samples_count = samples_count

        # Hold a reference to the device, to ensure that every iio.Buffer object
        # is destroyed before its corresponding IIO context.
        self._dev = device

    def __del__(self):
        """Destroy this buffer."""
        self.close()

    def close(self):
        """Synchronously destroy this buffer; repeated calls are harmless."""
        buffer = self._buffer
        self._buffer = None
        if buffer is not None:
            _buffer_destroy(buffer)

    def __len__(self):
        """Size of this buffer, in bytes."""
        return self._length

    def refill(self):
        """Fetch a new set of samples from the hardware."""
        _buffer_refill(self._buffer)

    def push(self, samples_count=None):
        """
        Submit the samples contained in this buffer to the hardware.

        :param samples_count: type=int
            The number of samples to submit, default = full buffer
        """
        _buffer_push_partial(self._buffer, samples_count or self._samples_count)

    def read(self):
        """
        Retrieve the samples contained inside the Buffer object.

        returns: type=bytearray
            An array containing the samples
        """
        start = _buffer_start(self._buffer)
        end = _buffer_end(self._buffer)
        array = bytearray(end - start)
        mytype = c_char * len(array)
        c_array = mytype.from_buffer(array)
        _memmove(c_array, start, len(array))
        return array

    def write(self, array):
        """
        Copy the given array of samples inside the Buffer object.

        :param array: type=bytearray
                The array containing the samples to copy

        returns: type=int
            The number of bytes written into the buffer
        """
        start = _buffer_start(self._buffer)
        end = _buffer_end(self._buffer)
        length = end - start
        if length > len(array):
            length = len(array)
        mytype = c_char * len(array)
        c_array = mytype.from_buffer(array)
        _memmove(start, c_array, length)
        return length

    def cancel(self):
        """Cancel the current buffer."""
        _buffer_cancel(self._buffer)

    def set_blocking_mode(self, blocking):
        """
        Set the buffer's blocking mode.

        :param blocking: type=boolean
            True if in blocking_mode else False.

        returns: type=int
            Return code from the C layer.
        """
        return _buffer_set_blocking_mode(self._buffer, c_bool(blocking))

    @property
    def device(self):
        """
        Device for the buffer.
        type: iio.Device
        """
        return self._dev

    @property
    def poll_fd(self):
        """
        Poll_fd for the buffer.
        type: int
        """
        return _buffer_get_poll_fd(self._buffer)

    @property
    def step(self):
        """
        Step size for the buffer.
        type: int
        """
        return _buffer_step(self._buffer)


class MetadataBuffer(Buffer):
    """An input buffer whose refill atomically returns frame metadata.

    This opt-in buffer requires an iiOD server advertising the matching
    extension.  IQ remains available through the ordinary Buffer and Channel
    read methods; ``refill()`` returns only the opaque metadata record paired
    with that IQ refill.  ``request`` is a required provider-owned session
    request; unsupported requests fail rather than silently degrading.
    """

    def __init__(
        self,
        device,
        samples_count,
        request,
        metadata_capacity=64 * 1024,
        batch_frames=1,
        ddr_burst_bytes=0,
        ddr_ring_bytes=0,
        ddr_ring_frames=0,
        ddr_ring_continuous=False,
        direct_async_frames=0,
    ):
        self._buffer = None
        if metadata_capacity <= 0:
            raise ValueError("metadata_capacity must be positive")
        if isinstance(batch_frames, bool) or not isinstance(batch_frames, int):
            raise TypeError("batch_frames must be an integer")
        if not 1 <= batch_frames <= 64:
            raise ValueError("batch_frames must be in [1, 64]")
        if isinstance(direct_async_frames, bool) or not isinstance(
            direct_async_frames, int
        ):
            raise TypeError("direct_async_frames must be an integer")
        if not 0 <= direct_async_frames <= 64:
            raise ValueError("direct_async_frames must be in [0, 64]")
        if direct_async_frames and batch_frames != 1:
            raise ValueError("direct async capture requires batch_frames=1")
        batch_cache_bytes = 0
        if batch_frames > 1:
            batch_cache_bytes = batch_frames * (
                samples_count * device.sample_size
                + int(metadata_capacity)
                + 2 * _sizeof(c_size_t)
            )
        if batch_cache_bytes > 64 * 1024 * 1024:
            raise ValueError("metadata batch cache exceeds the 64 MiB limit")
        # Device DDR burst and host-side metadata batching are separate cache
        # owners. Combining them would prequeue reads beyond the sealed device
        # burst and make teardown ambiguous.
        if isinstance(ddr_burst_bytes, bool) or not isinstance(
            ddr_burst_bytes, int
        ):
            raise TypeError("ddr_burst_bytes must be an integer")
        if ddr_burst_bytes < 0:
            raise ValueError("ddr_burst_bytes must not be negative")
        if ddr_burst_bytes and batch_frames != 1:
            raise ValueError("device DDR burst requires batch_frames=1")
        if isinstance(ddr_ring_bytes, bool) or not isinstance(ddr_ring_bytes, int):
            raise TypeError("ddr_ring_bytes must be an integer")
        if isinstance(ddr_ring_frames, bool) or not isinstance(ddr_ring_frames, int):
            raise TypeError("ddr_ring_frames must be an integer")
        if not isinstance(ddr_ring_continuous, bool):
            raise TypeError("ddr_ring_continuous must be a bool")
        if ddr_ring_bytes < 0 or ddr_ring_frames < 0:
            raise ValueError("DDR ring values must not be negative")
        if ddr_burst_bytes and ddr_ring_bytes:
            raise ValueError("DDR burst and DDR ring are mutually exclusive")
        if direct_async_frames and ddr_burst_bytes:
            raise ValueError("direct async capture cannot use the sealed DDR burst")
        if ddr_ring_bytes and batch_frames != 1:
            raise ValueError("device DDR ring requires batch_frames=1")
        if ddr_ring_bytes:
            if direct_async_frames and (ddr_ring_frames or ddr_ring_continuous):
                raise ValueError(
                    "direct async RAM extension owns the finite frame target"
                )
            if ddr_ring_continuous and ddr_ring_frames:
                raise ValueError(
                    "continuous DDR ring must not specify ddr_ring_frames"
                )
            if (
                not direct_async_frames
                and not ddr_ring_continuous
                and not ddr_ring_frames
            ):
                raise ValueError(
                    "finite DDR ring requires a positive ddr_ring_frames"
                )
        elif ddr_ring_frames or ddr_ring_continuous:
            raise ValueError("DDR ring mode requires positive ddr_ring_bytes")
        try:
            request = bytes(request)
        except (TypeError, ValueError) as exc:
            raise TypeError("request must be bytes-like") from exc
        if not request:
            raise ValueError("request must not be empty")
        ddr_burst_frames = 0
        ddr_burst_admitted_bytes = 0
        ddr_ring_admitted_bytes = 0
        ddr_ring_capacity_frames = 0
        if direct_async_frames:
            if _buffer_set_metadata_read_prequeue_async is None:
                raise OSError(
                    _ENOSYS,
                    "loaded libiio does not support direct async capture",
                )
            context_attrs = getattr(getattr(device, "_ctx", None), "attrs", {})
            if context_attrs.get("iio,buffer-direct-async") != "1":
                raise OSError("IIO context does not advertise direct async capture")
            if (
                ddr_ring_bytes
                and context_attrs.get("iio,buffer-direct-async-ring") != "1"
            ):
                raise OSError(
                    "IIO context does not advertise direct async RAM extension"
                )
        if ddr_burst_bytes:
            context_attrs = getattr(getattr(device, "_ctx", None), "attrs", {})
            if context_attrs.get("iio,buffer-ddr-burst") != "1":
                raise OSError("IIO context does not advertise device DDR burst v1")
            try:
                maximum_bytes = int(
                    context_attrs["iio,buffer-ddr-burst-max-iq-bytes"]
                )
            except (KeyError, TypeError, ValueError) as exc:
                raise OSError(
                    "IIO context has an invalid DDR burst byte limit"
                ) from exc
            frame_bytes = int(samples_count) * int(device.sample_size)
            if frame_bytes <= 0 or ddr_burst_bytes < frame_bytes:
                raise ValueError(
                    "ddr_burst_bytes must hold at least one complete frame"
                )
            if ddr_burst_bytes > maximum_bytes:
                raise ValueError("ddr_burst_bytes exceeds the advertised device limit")
            ddr_burst_frames = ddr_burst_bytes // frame_bytes
            ddr_burst_admitted_bytes = ddr_burst_frames * frame_bytes
            request += _struct.pack(
                "<IHHIIQQ",
                0x42524653,
                1,
                32,
                1,
                0,
                ddr_burst_bytes,
                0,
            )
        if ddr_ring_bytes:
            context_attrs = getattr(getattr(device, "_ctx", None), "attrs", {})
            if context_attrs.get("iio,buffer-ddr-ring") != "1":
                raise OSError("IIO context does not advertise device DDR ring v1")
            try:
                maximum_bytes = int(
                    context_attrs["iio,buffer-ddr-ring-max-iq-bytes"]
                )
            except (KeyError, TypeError, ValueError) as exc:
                raise OSError("IIO context has an invalid DDR ring byte limit") from exc
            frame_bytes = int(samples_count) * int(device.sample_size)
            if frame_bytes <= 0 or ddr_ring_bytes < frame_bytes:
                raise ValueError(
                    "ddr_ring_bytes must hold at least one complete frame"
                )
            if ddr_ring_bytes > maximum_bytes:
                raise ValueError("ddr_ring_bytes exceeds the advertised device limit")
            ddr_ring_capacity_frames = ddr_ring_bytes // frame_bytes
            ddr_ring_admitted_bytes = ddr_ring_capacity_frames * frame_bytes
            request += _struct.pack(
                "<IHHIIQQQQ",
                0x52524653,
                1,
                48,
                1,
                (
                    4
                    if direct_async_frames
                    else 2
                    if ddr_ring_continuous
                    else 1
                ),
                ddr_ring_bytes,
                ddr_ring_frames,
                0,
                0,
            )
        if len(request) > 4096:
            raise ValueError("request exceeds the 4096-byte libiio limit")
        request_storage = create_string_buffer(request, len(request))
        try:
            self._buffer = _create_buffer_with_metadata(
                device._device, samples_count, request_storage, len(request)
            )
            _buffer_set_metadata_batch_size(self._buffer, batch_frames)
            if direct_async_frames:
                _buffer_set_metadata_read_prequeue_async(
                    self._buffer,
                    direct_async_frames,
                    int(metadata_capacity),
                )
        except Exception:
            if self._buffer is not None:
                _buffer_destroy(self._buffer)
            self._buffer = None
            raise
        self._length = samples_count * device.sample_size
        self._samples_count = samples_count
        self._metadata_capacity = int(metadata_capacity)
        self._batch_frames = batch_frames
        self._batch_cache_bytes = batch_cache_bytes
        self._direct_async_frames = direct_async_frames
        self._direct_async_ring_extension = bool(
            direct_async_frames and ddr_ring_bytes
        )
        self._ddr_burst_requested_bytes = ddr_burst_bytes
        self._ddr_burst_admitted_bytes = ddr_burst_admitted_bytes
        self._ddr_burst_frames = ddr_burst_frames
        self._ddr_ring_requested_bytes = ddr_ring_bytes
        self._ddr_ring_admitted_bytes = ddr_ring_admitted_bytes
        self._ddr_ring_capacity_frames = ddr_ring_capacity_frames
        self._ddr_ring_capture_frames = ddr_ring_frames
        self._ddr_ring_continuous = ddr_ring_continuous
        self._metadata = None
        self._dev = device

    def refill(self):
        """Fetch IQ and return its atomically associated metadata bytes.

        With ``batch_frames > 1``, the first call drains that many consecutive
        wire responses into bounded host memory before returning frame zero.
        The following calls replay the remaining cached frames one at a time.
        """
        storage = create_string_buffer(self._metadata_capacity)
        metadata_bytes = c_size_t()
        _buffer_refill_with_metadata(
            self._buffer,
            storage,
            self._metadata_capacity,
            _byref(metadata_bytes),
        )
        self._metadata = storage.raw[: metadata_bytes.value]
        return self._metadata

    @property
    def metadata(self):
        """Metadata from the most recent successful refill, or ``None``."""
        return self._metadata

    @property
    def batch_frames(self):
        """Number of wire reads drained by each host-side refill batch."""
        return self._batch_frames

    @property
    def batch_cache_bytes(self):
        """Configured retained-cache bound (zero when batching is disabled)."""
        return self._batch_cache_bytes

    @property
    def direct_async_frames(self):
        """Finite DMA-to-network frame count, or zero when disabled."""
        return self._direct_async_frames

    @property
    def direct_async_ring_extension(self):
        """Whether RAM slots extend the bounded direct DMA FIFO."""
        return self._direct_async_ring_extension

    @property
    def ddr_burst_enabled(self):
        """Whether this buffer uses the opt-in device DDR burst cache."""
        return self._ddr_burst_requested_bytes > 0

    @property
    def ddr_burst_requested_bytes(self):
        """User-requested device IQ-cache byte budget."""
        return self._ddr_burst_requested_bytes

    @property
    def ddr_burst_admitted_bytes(self):
        """IQ bytes admitted after rounding down to complete IIO frames."""
        return self._ddr_burst_admitted_bytes

    @property
    def ddr_burst_frames(self):
        """Number of complete frames retained in device DDR."""
        return self._ddr_burst_frames

    @property
    def ddr_ring_enabled(self):
        """Whether this buffer uses the opt-in streaming device DDR ring."""
        return self._ddr_ring_requested_bytes > 0

    @property
    def ddr_ring_requested_bytes(self):
        """User-requested device ring IQ byte budget."""
        return self._ddr_ring_requested_bytes

    @property
    def ddr_ring_admitted_bytes(self):
        """Ring IQ bytes admitted after rounding down to whole frames."""
        return self._ddr_ring_admitted_bytes

    @property
    def ddr_ring_capacity_frames(self):
        """Number of complete IQ frames in the bounded device ring."""
        return self._ddr_ring_capacity_frames

    @property
    def ddr_ring_capture_frames(self):
        """Finite capture target, or zero for continuous mode."""
        return self._ddr_ring_capture_frames

    @property
    def ddr_ring_continuous(self):
        """Whether capture continues until the buffer is closed."""
        return self._ddr_ring_continuous

    def ddr_ring_status(self):
        """Return an atomic snapshot of the device DDR ring state."""
        if not self.ddr_ring_enabled:
            raise ValueError("this buffer does not use the device DDR ring")
        storage = create_string_buffer(128)
        status_bytes = _buffer_get_metadata_status(self._buffer, storage, 128)
        if status_bytes != 128:
            raise OSError("IIO server returned an invalid DDR ring status size")
        values = _struct.unpack("<IHHIIIi13Q", storage.raw)
        if values[:3] != (0x53524653, 1, 128) or values[-2:] != (0, 0):
            raise OSError("IIO server returned an invalid DDR ring status")
        state_names = (
            "off",
            "reserved",
            "running",
            "draining",
            "complete",
            "failed",
            "cancelled",
        )
        reason_names = (
            "none",
            "target_complete",
            "client_cancelled",
            "client_disconnected",
            "consumer_stall",
            "dma_error",
            "counter_gap",
            "transport_error",
            "internal_error",
        )
        state, reason, valid_fields, error_code = values[3:7]
        if state >= len(state_names) or reason >= len(reason_names):
            raise OSError("IIO server returned an invalid DDR ring state")
        counters = values[7:-2]
        return {
            "state": state_names[state],
            "terminal_reason": reason_names[reason],
            "error_code": error_code,
            "requested_capacity_iq_bytes": counters[0],
            "admitted_capacity_iq_bytes": counters[1],
            "target_frames": counters[2],
            "produced_frames": counters[3],
            "consumed_frames": counters[4],
            "high_water_frames": counters[5],
            "wrap_count": counters[6],
            "producer_position": counters[7],
            "consumer_position": counters[8],
            "last_contiguous_sample_sequence": (
                counters[9] if valid_fields & 1 else None
            ),
            "first_unavailable_sample_sequence": (
                counters[10] if valid_fields & 2 else None
            ),
        }


class _DeviceOrTrigger(object):
    def __init__(self, _ctx, _device):
        self._ctx = _ctx
        self._device = _device
        self._context = _d_get_context(_device)
        self._attrs = {
            name: DeviceAttr(_device, name)
            for name in [
                _d_get_attr(_device, x).decode("ascii")
                for x in range(0, _d_attr_count(_device))
            ]
        }
        self._debug_attrs = {
            name: DeviceDebugAttr(_device, name)
            for name in [
                _d_get_debug_attr(_device, x).decode("ascii")
                for x in range(0, _d_debug_attr_count(_device))
            ]
        }
        self._buffer_attrs = {
            name: DeviceBufferAttr(_device, name)
            for name in [
                _d_get_buffer_attr(_device, x).decode("ascii")
                for x in range(0, _d_buffer_attr_count(_device))
            ]
        }

        self._id = _d_get_id(self._device).decode("ascii")

        name_raw = _d_get_name(self._device)
        self._name = name_raw.decode("ascii") if name_raw is not None else None

        label_raw = _d_get_label(self._device)
        self._label = label_raw.decode("ascii") if label_raw is not None else None

    def reg_write(self, reg, value):
        """
        Set a value to one register of this device.

        :param reg: type=int
            The register address
        :param value: type=int
            The value that will be used for this register
        """
        _d_reg_write(self._device, reg, value)

    def reg_read(self, reg):
        """
        Read the content of a register of this device.

        :param reg: type=int
            The register address

        returns: type=int
            The value of the register
        """
        value = c_uint()
        _d_reg_read(self._device, reg, _byref(value))
        return value.value

    def find_channel(self, name_or_id, is_output=False):
        """

        Find a IIO channel by its name or ID.

        :param name_or_id: type=str
            The name or ID of the channel to find
        :param is_output: type=bool
            Set to True to search for an output channel

        returns: type=iio.Device or type=iio.Trigger
            The IIO Device
        """
        chn = _d_find_channel(self._device, name_or_id.encode("ascii"), is_output)
        return None if bool(chn) is False else Channel(self, chn)

    def set_kernel_buffers_count(self, count):
        """

        Set the number of kernel buffers to use with the specified device.

        :param count: type=int
            The number of kernel buffers

        """
        return _d_set_buffers_count(self._device, count)

    @property
    def kernel_buffers_count(self):
        """Number of kernel buffers configured for this device."""
        return _d_get_buffers_count(self._device)

    @property
    def sample_size(self):
        """
        Sample size of this device.
        type: int

        The sample size varies each time channels get enabled or disabled.
        """
        return _get_sample_size(self._device)

    id = property(
        lambda self: self._id,
        None,
        None,
        "An identifier of this device, only valid in this IIO context.\n\ttype=str",
    )
    name = property(
        lambda self: self._name, None, None, "The name of this device.\n\ttype=str"
    )
    label = property(
        lambda self: self._label, None, None, "The label of this device.\n\ttype=str",
    )
    attrs = property(
        lambda self: self._attrs,
        None,
        None,
        "List of attributes for this IIO device.\n\ttype=dict of iio.DeviceAttr",
    )
    debug_attrs = property(
        lambda self: self._debug_attrs,
        None,
        None,
        "List of debug attributes for this IIO device.\n\ttype=dict of iio.DeviceDebugAttr",
    )
    buffer_attrs = property(
        lambda self: self._buffer_attrs,
        None,
        None,
        "List of buffer attributes for this IIO device.\n\ttype=dict of iio.DeviceBufferAttr",
    )
    channels = property(
        lambda self: sorted([
            Channel(self, _get_channel(self._device, x))
            for x in range(0, _channels_count(self._device))
        ], key=lambda c: c.id),
        None,
        None,
        "List of channels available with this IIO device.\n\ttype=list of iio.Channel objects",
    )


class Trigger(_DeviceOrTrigger):
    """Contains the representation of an IIO device that can act as a trigger."""

    def __init__(self, ctx, _device):
        """
        Initialize a new instance of the Trigger class.

        :param _device: type=iio._DevicePtr
            A pointer to an IIO device.

        returns: type=iio.Trigger
            An new instance of this class
        """
        super(Trigger, self).__init__(ctx, _device)

    def _get_rate(self):
        return int(self._attrs["sampling_frequency"].value)

    def _set_rate(self, value):
        self._attrs["sampling_frequency"].value = str(value)

    frequency = property(
        _get_rate,
        _set_rate,
        None,
        "Configured frequency (in Hz) of this trigger\n\ttype=int",
    )


class Device(_DeviceOrTrigger):
    """Contains the representation of an IIO device."""

    def __init__(self, ctx, _device):
        """
        Initialize a new instance of the Device class.

        :param ctx: type=iio.Context
            A valid instance of the iio.Context class.
        :param _device: type=_DevicePtr
            A pointer to an IIO device.

        returns: type=iio.Device
            An new instance of this class
        """
        super(Device, self).__init__(ctx, _device)
        self.ctx = ctx

    def _set_trigger(self, trigger):
        _d_set_trigger(self._device, trigger._device if trigger else None)

    def _get_trigger(self):
        value = _DevicePtr()
        _d_get_trigger(self._device, _byref(value))
        trig = Trigger(value.contents)

        for dev in self.ctx.devices:
            if trig.id == dev.id:
                return dev
        return None

    trigger = property(
        _get_trigger,
        _set_trigger,
        None,
        "Contains the configured trigger for this IIO device.\n\ttype=iio.Trigger",
    )
    hwmon = property(
        lambda self: self._id[:5] == "hwmon", None, None,
        "Contains True if the device is a hardware-monitoring device, False if it is a IIO device.\n\ttype=bool",
    )

    @property
    def context(self):
        """
        Context for the device.
        type: iio.Context
        """
        return self.ctx


class Context(object):
    """Contains the representation of an IIO context."""

    def __init__(self, _context=None):
        """
        Initialize a new instance of the Context class, using the local or the network backend of the IIO library.

        returns: type=iio.Context
            An new instance of this class

        This function will create a network context if the IIOD_REMOTE
        environment variable is set to the hostname where the IIOD server runs.
        If set to an empty string, the server will be discovered using ZeroConf.
        If the environment variable is not set, a local context will be created instead.
        """
        self._context = None

        if _context is None:
            self._context = _new_default()
        elif _isstring(_context):
            self._context = _new_uri(_context.encode("ascii"))
        else:
            self._context = _context

        self._attrs = {}
        for index in range(0, _get_attrs_count(self._context)):
            str1 = c_char_p()
            str2 = c_char_p()
            _get_attr(self._context, index, _byref(str1), _byref(str2))
            self._attrs[str1.value.decode("ascii")] = str2.value.decode("ascii")

        self._name = _get_name(self._context).decode("ascii")
        self._description = _get_description(self._context).decode("ascii")
        self._xml = _get_xml(self._context).decode("ascii")

        major = c_uint()
        minor = c_uint()
        buf = create_string_buffer(8)
        _get_version(self._context, _byref(major), _byref(minor), buf)
        self._version = (major.value, minor.value, buf.value.decode("ascii"))

    def __del__(self):
        """Destroy this context."""
        self.close()

    def close(self):
        """Synchronously destroy this context; repeated calls are harmless."""
        context = self._context
        self._context = None
        if context is not None:
            _destroy(context)

    def set_timeout(self, timeout):
        """
        Set a timeout for I/O operations.

        :param timeout: type=int
            The timeout value, in milliseconds
        """
        _set_timeout(self._context, timeout)

    def clone(self):
        """
        Clone this instance.

        returns: type=iio.LocalContext
            An new instance of this class
        """
        return Context(_clone(self._context))

    def find_device(self, name_or_id_or_label):
        """

        Find a IIO device by its name, ID or label.

        :param name_or_id_or_label: type=str
            The name, ID or label of the device to find

        returns: type=iio.Device or type=iio.Trigger
            The IIO Device
        """
        dev = _find_device(self._context, name_or_id_or_label.encode("ascii"))
        return None if bool(dev) is False else Trigger(self, dev) if _d_is_trigger(dev) else Device(self, dev)

    name = property(
        lambda self: self._name, None, None, "Name of this IIO context.\n\ttype=str"
    )
    description = property(
        lambda self: self._description,
        None,
        None,
        "Description of this IIO context.\n\ttype=str",
    )
    xml = property(
        lambda self: self._xml,
        None,
        None,
        "XML representation of the current context.\n\ttype=str",
    )
    version = property(
        lambda self: self._version,
        None,
        None,
        "Version of the backend.\n\ttype=(int, int, str)",
    )
    attrs = property(
        lambda self: self._attrs,
        None,
        None,
        "List of context-specific attributes\n\ttype=dict of str objects",
    )
    devices = property(
        lambda self: [
            Trigger(self, dev) if _d_is_trigger(dev) else Device(self, dev)
            for dev in [
                _get_device(self._context, x)
                for x in range(0, _devices_count(self._context))
            ]
        ],
        None,
        None,
        "List of devices contained in this context.\n\ttype=list of iio.Device and iio.Trigger objects",
    )


class LocalContext(Context):
    """Local IIO Context."""

    def __init__(self):
        """
        Initialize a new instance of the Context class, using the local backend of the IIO library.

        returns: type=iio.LocalContext
            An new instance of this class
        """
        ctx = _new_local()
        super(LocalContext, self).__init__(ctx)


class XMLContext(Context):
    """XML IIO Context."""

    def __init__(self, xmlfile):
        """
        Initialize a new instance of the Context class, using the XML backend of the IIO library.

        :param xmlfile: type=str
            Filename of the XML file to build the context from

        returns: type=iio.XMLContext
            An new instance of this class
        """
        ctx = _new_xml(xmlfile.encode("ascii"))
        super(XMLContext, self).__init__(ctx)


class NetworkContext(Context):
    """Network IIO Context."""

    def __init__(self, hostname=None):
        """
        Initialize a new instance of the Context class, using the network backend of the IIO library.

        :param hostname: type=str
            Hostname, IPv4 or IPv6 address where the IIO Daemon is running

        returns: type=iio.NetworkContext
            An new instance of this class
        """
        ctx = _new_network(hostname.encode("ascii") if hostname is not None else None)
        super(NetworkContext, self).__init__(ctx)


def scan_contexts():
    """Scan Context."""
    scan_ctx = dict()
    ptr = _POINTER(_ContextInfoPtr)()

    ctx = _create_scan_context(None, 0)
    ctx_nb = _get_context_info_list(ctx, _byref(ptr))

    for i in range(0, ctx_nb):
        scan_ctx[
            _context_info_get_uri(ptr[i]).decode("ascii")
        ] = _context_info_get_description(ptr[i]).decode("ascii")

    _context_info_list_free(ptr)
    _destroy_scan_context(ctx)
    return scan_ctx
