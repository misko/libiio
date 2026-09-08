"""Real libiio network backend against a bounded loopback-only protocol peer.

This peer does not emulate RF or the provider's classifier. The C server test
separately exercises the real iiOD parser/handler; these tests check network
capability gating, framing, response poisoning and the public Python/C APIs.
"""

from contextlib import contextmanager
import errno
import socketserver
import threading

import pytest

import iio


def context_xml(drain_capability):
    capability = (
        "" if drain_capability is None else
        f'<context-attribute name="iio,buffer-metadata-drain" value="{drain_capability}"/>'
    )
    return (
        '<!DOCTYPE context ['
        '<!ELEMENT context (context-attribute | device)*>'
        '<!ATTLIST context name CDATA #REQUIRED>'
        '<!ELEMENT context-attribute EMPTY>'
        '<!ATTLIST context-attribute name CDATA #REQUIRED value CDATA #REQUIRED>'
        '<!ELEMENT device (channel*)><!ATTLIST device id CDATA #REQUIRED>'
        '<!ELEMENT channel (scan-element)>'
        '<!ATTLIST channel id CDATA #REQUIRED type CDATA #REQUIRED>'
        '<!ELEMENT scan-element EMPTY>'
        '<!ATTLIST scan-element index CDATA #REQUIRED format CDATA #REQUIRED>]>'
        '<context name="network">'
        '<context-attribute name="iio,buffer-metadata" value="3"/>'
        f'{capability}<device id="dev0"><channel id="voltage0" type="input">'
        '<scan-element index="0" format="le:S16/16&gt;&gt;0"/>'
        '</channel></device></context>'
    ).encode()


class Peer(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, capability, replies, drain_capacity):
        self.xml = context_xml(capability)
        self.replies = list(replies)
        self.commands = []
        self.errors = []
        self.requests = []
        self.drain_capacity = drain_capacity
        super().__init__(("127.0.0.1", 0), Handler)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        try:
            while True:
                line = self.rfile.readline(1024)
                if not line:
                    return
                command = line.strip()
                if not command:
                    continue
                self.server.commands.append(command)
                if command == b"PRINT":
                    self.wfile.write(str(len(self.server.xml)).encode() + b"\n")
                    self.wfile.write(self.server.xml + b"\n")
                elif command == b"VERSION":
                    self.wfile.write(b"0.25.test000\n")
                elif command.startswith(b"TIMEOUT "):
                    self.wfile.write(b"0\n")
                elif command.startswith(b"OPENM "):
                    parts = command.split()
                    assert parts[1:4] == [b"dev0", b"8", b"00000001"]
                    self.server.requests.append(self.rfile.read(int(parts[4])))
                    self.wfile.write(b"0\n")
                elif command.startswith(b"READBUFM "):
                    assert command == b"READBUFM dev0 16 64"
                    self.wfile.write(b"16\n00000001\n6\nlegacy" + bytes(range(16)))
                elif command.startswith(b"DRAINBUFM "):
                    assert command == f"DRAINBUFM dev0 {self.server.drain_capacity}".encode()
                    self.wfile.write(self.server.replies.pop(0))
                    if self.server.replies == [b"DISCONNECT"]:
                        self.server.replies.pop()
                        return
                elif command == b"CLOSE dev0":
                    self.wfile.write(b"0\n")
                elif command == b"EXIT":
                    return
                else:
                    raise AssertionError(f"unexpected command: {command!r}")
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected after the client rejects a malformed response.
        except Exception as error:
            self.server.errors.append(error)


@contextmanager
def peer_buffer(capability="1", replies=(), drain_capacity=64):
    with Peer(capability, replies, drain_capacity) as peer:
        thread = threading.Thread(target=peer.serve_forever, kwargs={"poll_interval": 0.01})
        thread.start()
        context = buffer = None
        try:
            context = iio.Context(f"ip:127.0.0.1:{peer.server_address[1]}")
            context.set_timeout(1000)
            device = context.find_device("dev0")
            device.channels[0].enabled = True
            buffer = iio.MetadataBuffer(device, 8, b"opt-in-test", metadata_capacity=64)
            yield peer, buffer
        finally:
            if buffer is not None:
                buffer.close()
            if context is not None:
                context.close()
            peer.shutdown()
            thread.join(timeout=3)
            assert not thread.is_alive()
            assert not peer.errors


def test_network_drain_roundtrip_and_iq_unchanged():
    with peer_buffer(replies=[b"-11\n", b"4\ntail", b"5\nFINAL", b"-61\n"]) as (peer, buffer):
        assert buffer.refill() == b"legacy"
        before = bytes(buffer.read())
        assert before == bytes(range(16))
        with pytest.raises(OSError) as error:
            buffer.drain_metadata(64)
        assert error.value.errno == errno.EAGAIN
        assert buffer.drain_metadata(64) == b"tail"
        assert buffer.drain_metadata(64) == b"FINAL"
        with pytest.raises(OSError) as error:
            buffer.drain_metadata(64)
        assert error.value.errno == errno.ENODATA
        assert bytes(buffer.read()) == before
        assert buffer.metadata == b"legacy"
        assert peer.requests == [b"opt-in-test"]
        assert sum(command.startswith(b"READBUFM ") for command in peer.commands) == 1
        assert sum(command.startswith(b"OPENM ") for command in peer.commands) == 1


def test_network_maximum_binary_metadata_is_opaque():
    payload = bytes(range(256)) * 256
    with peer_buffer(replies=[b"65536\n" + payload], drain_capacity=65536) as (peer, buffer):
        assert buffer.drain_metadata(65536) == payload
        assert not any(command.startswith(b"READBUFM ") for command in peer.commands)


@pytest.mark.parametrize("capability", [None, "0", "2"])
def test_network_unsupported_drain_does_not_send_command(capability):
    with peer_buffer(capability) as (peer, buffer):
        with pytest.raises(OSError) as error:
            buffer.drain_metadata(64)
        assert error.value.errno == errno.ENOSYS
        assert buffer.refill() == b"legacy"
        assert not any(command.startswith(b"DRAINBUFM ") for command in peer.commands)


@pytest.mark.parametrize("reply", [b"0\n", b"65\n", b"4294967300\n", b"4junk\n", b"4\nab"])
def test_network_malformed_drain_invalidates_transport(reply):
    with peer_buffer(replies=[reply, b"DISCONNECT"]) as (peer, buffer):
        with pytest.raises(OSError):
            buffer.drain_metadata(64)
        with pytest.raises(OSError) as error:
            buffer.drain_metadata(64)
        assert error.value.errno == errno.EBADF
        assert sum(command.startswith(b"DRAINBUFM ") for command in peer.commands) == 1
