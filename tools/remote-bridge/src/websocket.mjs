import crypto from 'node:crypto';
import EventEmitter from 'node:events';

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const MAX_FRAME_PAYLOAD = 256 * 1024;

function encodeFrame(opcode, payloadBuffer) {
  const payload = Buffer.isBuffer(payloadBuffer) ? payloadBuffer : Buffer.from(payloadBuffer, 'utf8');
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.alloc(2);
    header[0] = 0x80 | opcode;
    header[1] = len;
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(len), 2);
  }
  return Buffer.concat([header, payload]);
}

export class WebSocketClient extends EventEmitter {
  constructor(socket, metadata = {}) {
    super();
    this.socket = socket;
    this.metadata = metadata;
    this.closed = false;
    this.lastPongAt = Date.now();
    this._buffer = Buffer.alloc(0);

    socket.on('data', (chunk) => this._onData(chunk));
    socket.on('close', () => this._handleClose());
    socket.on('error', () => this._handleClose());
  }

  sendJson(obj) {
    this.sendText(JSON.stringify(obj));
  }

  sendText(text) {
    if (this.closed || this.socket.destroyed) return;
    try {
      this.socket.write(encodeFrame(0x1, Buffer.from(text, 'utf8')));
    } catch {
      this._handleClose();
    }
  }

  ping() {
    if (this.closed || this.socket.destroyed) return;
    try {
      this.socket.write(encodeFrame(0x9, Buffer.alloc(0)));
    } catch {
      this._handleClose();
    }
  }

  close(code = 1000, reason = '') {
    if (this.closed) return;
    this.closed = true;
    try {
      const reasonBuf = Buffer.from(reason, 'utf8');
      const body = Buffer.alloc(2 + reasonBuf.length);
      body.writeUInt16BE(code, 0);
      reasonBuf.copy(body, 2);
      if (!this.socket.destroyed) {
        this.socket.write(encodeFrame(0x8, body));
        this.socket.end();
      }
    } catch {
      this.socket.destroy();
    }
    this.emit('close');
  }

  _handleClose() {
    if (this.closed) return;
    this.closed = true;
    this.emit('close');
  }

  _onData(chunk) {
    this._buffer = Buffer.concat([this._buffer, chunk]);
    while (this._buffer.length >= 2) {
      const first = this._buffer[0];
      const second = this._buffer[1];
      const opcode = first & 0x0f;
      const masked = (second & 0x80) !== 0;
      let payloadLen = second & 0x7f;
      let offset = 2;

      if (payloadLen === 126) {
        if (this._buffer.length < 4) return;
        payloadLen = this._buffer.readUInt16BE(2);
        offset = 4;
      } else if (payloadLen === 127) {
        if (this._buffer.length < 10) return;
        const bigLen = this._buffer.readBigUInt64BE(2);
        if (bigLen > BigInt(MAX_FRAME_PAYLOAD)) {
          this.close(1009, 'Message too large');
          return;
        }
        payloadLen = Number(bigLen);
        offset = 10;
      }

      if (payloadLen > MAX_FRAME_PAYLOAD) {
        this.close(1009, 'Message too large');
        return;
      }

      const maskLen = masked ? 4 : 0;
      if (this._buffer.length < offset + maskLen + payloadLen) return;

      const maskKey = masked ? this._buffer.subarray(offset, offset + 4) : null;
      const payloadStart = offset + maskLen;
      const rawPayload = Buffer.from(this._buffer.subarray(payloadStart, payloadStart + payloadLen));
      this._buffer = this._buffer.subarray(payloadStart + payloadLen);

      if (masked && maskKey) {
        for (let i = 0; i < rawPayload.length; i++) {
          rawPayload[i] ^= maskKey[i & 3];
        }
      }

      if (opcode === 0x8) {
        this.close(1000);
        return;
      }
      if (opcode === 0x9) {
        if (!this.closed && !this.socket.destroyed) {
          this.socket.write(encodeFrame(0xA, rawPayload));
        }
        continue;
      }
      if (opcode === 0xA) {
        this.lastPongAt = Date.now();
        continue;
      }
      if (opcode === 0x1) {
        this.emit('message', rawPayload.toString('utf8'));
      }
    }
  }
}

export class WebSocketHub extends EventEmitter {
  constructor() {
    super();
    this.clients = new Set();
    this._heartbeat = setInterval(() => {
      const now = Date.now();
      for (const client of this.clients) {
        if (now - client.lastPongAt > 75_000) {
          client.close(1001, 'Heartbeat timeout');
          continue;
        }
        client.ping();
      }
    }, 25_000);
    this._heartbeat.unref();
  }

  handleUpgrade(req, socket, head, metadata = {}) {
    const key = req.headers['sec-websocket-key'];
    if (!key || typeof key !== 'string') {
      socket.write('HTTP/1.1 400 Bad Request\r\n\r\n');
      socket.destroy();
      return null;
    }

    const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
    );

    const client = new WebSocketClient(socket, metadata);
    this.clients.add(client);
    client.on('close', () => {
      this.clients.delete(client);
      this.emit('disconnection', client);
    });
    if (head && head.length > 0) {
      client._onData(head);
    }
    this.emit('connection', client);
    return client;
  }

  broadcast(obj) {
    if (this.clients.size === 0) return;
    const text = JSON.stringify(obj);
    for (const client of this.clients) {
      client.sendText(text);
    }
  }

  closeAll() {
    clearInterval(this._heartbeat);
    for (const client of Array.from(this.clients)) {
      client.close(1001, 'Server shutting down');
    }
    this.clients.clear();
  }
}
