#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The UFS's queries over the kernel's BSG node, as the Flipper OS installer
makes them (its src/core/ufs.rs): descriptors, attributes and flags read
and written, and a device provisioned as its "Reprovision UFS" does it. It
runs on Flipper OS as root, and is the Linux check's way to the UFS's
configuration (sent in over the console). Standard library only.

    python3 ufs_provision.py desc IDN [INDEX]      a descriptor, in hex
    python3 ufs_provision.py attr IDN [VALUE]      an attribute (written)
    python3 ufs_provision.py flag IDN              a flag
    python3 ufs_provision.py provision HEX [BOOT]  a configuration descriptor
                                                   (index 0) written, then
    bBootLunEn (1: A, the default), fDeviceInit set and waited for (the
    device applies the configuration), the host's data LUs deleted and
    scanned for again; "provisioned" once a block device is back.
"""
import ctypes, fcntl, glob, os, struct, sys, time

SG_IO = 0x2285
NODE = "/dev/bsg/ufs-bsg0"
QUERY_REQ = 0x16                        # (UPIU's, and the BSG msgcode)
READ_DESC, WRITE_DESC, READ_ATTR, WRITE_ATTR, READ_FLAG, SET_FLAG = range(1, 7)
IDN_CONFIGURATION, ATTR_BOOT_LUN_EN, FLAG_DEVICE_INIT = 0x01, 0x00, 0x01


class SgIoV4(ctypes.Structure):         # include/uapi/linux/bsg.h
    _fields_ = [(n, t) for n, t in (
        ("guard", ctypes.c_int32), ("protocol", ctypes.c_uint32),
        ("subprotocol", ctypes.c_uint32), ("request_len", ctypes.c_uint32),
        ("request", ctypes.c_uint64), ("request_tag", ctypes.c_uint64),
        ("request_attr", ctypes.c_uint32), ("request_priority", ctypes.c_uint32),
        ("request_extra", ctypes.c_uint32), ("max_response_len", ctypes.c_uint32),
        ("response", ctypes.c_uint64), ("dout_iovec_count", ctypes.c_uint32),
        ("dout_xfer_len", ctypes.c_uint32), ("din_iovec_count", ctypes.c_uint32),
        ("din_xfer_len", ctypes.c_uint32), ("dout_xferp", ctypes.c_uint64),
        ("din_xferp", ctypes.c_uint64), ("timeout", ctypes.c_uint32),
        ("flags", ctypes.c_uint32), ("usr_ptr", ctypes.c_uint64),
        ("spare_in", ctypes.c_uint32), ("driver_status", ctypes.c_uint32),
        ("transport_status", ctypes.c_uint32), ("device_status", ctypes.c_uint32),
        ("retry_delay", ctypes.c_uint32), ("info", ctypes.c_uint32),
        ("duration", ctypes.c_uint32), ("response_len", ctypes.c_uint32),
        ("din_resid", ctypes.c_int32), ("dout_resid", ctypes.c_int32),
        ("generated_tag", ctypes.c_uint64), ("spare_out", ctypes.c_uint32),
        ("padding", ctypes.c_uint32))]


def query(fd, opcode, idn, index=0, length=0, out=None, value=0):
    """A QUERY REQUEST: the reply's UPIU, and what came in (a descriptor)"""
    write = opcode in (WRITE_DESC, WRITE_ATTR, SET_FLAG)
    req = bytearray(36)                 # struct ufs_bsg_request
    struct.pack_into("<I", req, 0, QUERY_REQ)
    req[4] = QUERY_REQ
    req[4 + 5] = 0x81 if write else 0x01
    req[4 + 12:4 + 16] = bytes((opcode, idn, index, 0))
    struct.pack_into(">H", req, 4 + 18, length)
    struct.pack_into(">I", req, 4 + 20, value)
    if out is not None:                 # (the data segment's length: ours)
        struct.pack_into(">H", req, 4 + 10, length)
    rq = ctypes.create_string_buffer(bytes(req), len(req))
    rp = ctypes.create_string_buffer(40)  # struct ufs_bsg_reply
    hdr = SgIoV4(guard=ord("Q"), subprotocol=2, request_len=len(req),
                 request=ctypes.addressof(rq), max_response_len=40,
                 response=ctypes.addressof(rp), timeout=30000)
    if out is not None:
        ob = ctypes.create_string_buffer(bytes(out), len(out))
        hdr.dout_xfer_len, hdr.dout_xferp = len(out), ctypes.addressof(ob)
    elif opcode == READ_DESC:
        ib = ctypes.create_string_buffer(length)
        hdr.din_xfer_len, hdr.din_xferp = length, ctypes.addressof(ib)
    fcntl.ioctl(fd, SG_IO, hdr)
    result = struct.unpack_from("<i", rp.raw, 0)[0]
    upiu = rp.raw[8:40]
    if result or upiu[6]:
        sys.exit(f"query {opcode:#x} idn {idn:#x}[{index}]: result {result}, "
                 f"response {upiu[6]:#04x}")
    return upiu, ib.raw if opcode == READ_DESC else None


def desc(fd, idn, index=0):
    data = query(fd, READ_DESC, idn, index, 255)[1]
    return data[:data[0]]


def rescan():
    """the host's data LUs gone, and scanned for again (the installer's
    rescan_logical_units: not its well-known LUs)"""
    host = next(os.path.basename(h) for h in glob.glob("/sys/class/scsi_host/*")
                if open(f"{h}/proc_name").read().strip() == "ufshcd")
    for dev in glob.glob(f"/sys/class/scsi_host/{host}/device/target*/*:*"):
        if int(dev.rsplit(":", 1)[1]) < 0xc100:
            with open(f"{dev}/delete", "w") as f:
                f.write("1\n")
    with open(f"/sys/class/scsi_host/{host}/scan", "w") as f:
        f.write("- - -\n")


fd = os.open(NODE, os.O_RDWR)
cmd, args = sys.argv[1], [int(a, 0) for a in sys.argv[2:]
                          if sys.argv[1] != "provision"]
if cmd == "desc":
    print(desc(fd, *args).hex())
elif cmd == "attr":
    if len(args) > 1:
        query(fd, WRITE_ATTR, args[0], value=args[1])
    print(struct.unpack(">I", query(fd, READ_ATTR, args[0])[0][20:24])[0])
elif cmd == "flag":
    print(query(fd, READ_FLAG, args[0])[0][23])
elif cmd == "provision":
    cfg = bytes.fromhex(sys.argv[2])
    boot = int(sys.argv[3], 0) if len(sys.argv) > 3 else 1
    query(fd, WRITE_DESC, IDN_CONFIGURATION, 0, len(cfg), cfg)
    query(fd, WRITE_ATTR, ATTR_BOOT_LUN_EN, value=boot)
    query(fd, SET_FLAG, FLAG_DEVICE_INIT)
    for _ in range(200):                # (the installer waits 10 s)
        if not query(fd, READ_FLAG, FLAG_DEVICE_INIT)[0][23]:
            break
        time.sleep(0.05)
    else:
        sys.exit("fDeviceInit not cleared")
    rescan()
    for _ in range(200):                # (and 20 s for a block device)
        if glob.glob("/sys/class/scsi_host/host*/device/target*/*:*/block/*"):
            print("provisioned")
            break
        time.sleep(0.1)
    else:
        sys.exit("no block device back")
else:
    sys.exit(__doc__)
