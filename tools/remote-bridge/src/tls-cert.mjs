import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

function derLen(length) {
  if (length < 0x80) return Buffer.from([length]);
  const bytes = [];
  let n = length;
  while (n > 0) {
    bytes.unshift(n & 0xff);
    n >>= 8;
  }
  return Buffer.from([0x80 | bytes.length, ...bytes]);
}

function derTag(tag, content) {
  const buf = Buffer.isBuffer(content) ? content : Buffer.concat(content);
  return Buffer.concat([Buffer.from([tag]), derLen(buf.length), buf]);
}

function derSeq(...items) {
  return derTag(0x30, items);
}

function derSet(...items) {
  return derTag(0x31, items);
}

function derInt(bufOrNum) {
  let buf = typeof bufOrNum === 'number'
    ? Buffer.from(bufOrNum.toString(16).padStart(bufOrNum.toString(16).length % 2 ? bufOrNum.toString(16).length + 1 : bufOrNum.toString(16).length, '0'), 'hex')
    : Buffer.from(bufOrNum);
  // Strip leading zeros while keeping positive sign bit clear
  while (buf.length > 1 && buf[0] === 0x00 && (buf[1] & 0x80) === 0) {
    buf = buf.subarray(1);
  }
  if (buf[0] & 0x80) {
    buf = Buffer.concat([Buffer.from([0x00]), buf]);
  }
  return derTag(0x02, buf);
}

function derBitString(buf) {
  return derTag(0x03, Buffer.concat([Buffer.from([0x00]), buf]));
}

function derOid(oidStr) {
  const parts = oidStr.split('.').map(Number);
  const out = [parts[0] * 40 + parts[1]];
  for (let i = 2; i < parts.length; i++) {
    let val = parts[i];
    const encoded = [val & 0x7f];
    val >>= 7;
    while (val > 0) {
      encoded.unshift((val & 0x7f) | 0x80);
      val >>= 7;
    }
    out.push(...encoded);
  }
  return derTag(0x06, Buffer.from(out));
}

function derUtf8(text) {
  return derTag(0x0c, Buffer.from(text, 'utf8'));
}

function derGeneralizedTime(date) {
  const pad = (n, w = 2) => String(n).padStart(w, '0');
  const s =
    pad(date.getUTCFullYear(), 4) +
    pad(date.getUTCMonth() + 1) +
    pad(date.getUTCDate()) +
    pad(date.getUTCHours()) +
    pad(date.getUTCMinutes()) +
    pad(date.getUTCSeconds()) +
    'Z';
  return derTag(0x18, Buffer.from(s, 'ascii'));
}

function derExplicit(tagNumber, content) {
  return derTag(0xa0 | tagNumber, content);
}

function generateSelfSignedEcdsaCert(commonName = 'Winmux Remote Bridge') {
  const { privateKey, publicKey } = crypto.generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const spkiDer = publicKey.export({ type: 'spki', format: 'der' });
  const keyPem = privateKey.export({ type: 'pkcs8', format: 'pem' });

  const serial = crypto.randomBytes(16);
  serial[0] &= 0x7f;
  if (serial[0] === 0) serial[0] = 1;

  // ecdsa-with-SHA256 OID: 1.2.840.10045.4.3.2
  const sigAlg = derSeq(derOid('1.2.840.10045.4.3.2'));
  // CN OID: 2.5.4.3
  const name = derSeq(derSet(derSeq(derOid('2.5.4.3'), derUtf8(commonName))));
  const now = new Date(Date.now() - 5 * 60 * 1000);
  const expires = new Date(Date.now() + 10 * 365 * 24 * 3600 * 1000);
  const validity = derSeq(derGeneralizedTime(now), derGeneralizedTime(expires));

  // BasicConstraints (2.5.29.19) = CA:FALSE; ExtendedKeyUsage (2.5.29.37) = serverAuth (1.3.6.1.5.5.7.3.1)
  const basicConstraintsExt = derSeq(derOid('2.5.29.19'), derTag(0x04, derSeq()));
  const ekuExt = derSeq(derOid('2.5.29.37'), derTag(0x04, derSeq(derOid('1.3.6.1.5.5.7.3.1'))));
  const extensions = derExplicit(3, derSeq(basicConstraintsExt, ekuExt));

  const tbs = derSeq(
    derExplicit(0, derInt(2)), // v3
    derInt(serial),
    sigAlg,
    name,
    validity,
    name,
    spkiDer,
    extensions
  );

  const signature = crypto.sign('sha256', tbs, privateKey);
  const certDer = derSeq(tbs, sigAlg, derBitString(signature));
  const b64 = certDer.toString('base64').match(/.{1,64}/g).join('\n');
  const certPem = `-----BEGIN CERTIFICATE-----\n${b64}\n-----END CERTIFICATE-----\n`;

  // Validate with Node's native X509Certificate parser.
  const x509 = new crypto.X509Certificate(certPem);
  const fingerprintSha256 = x509.fingerprint256.replaceAll(':', '').toLowerCase();

  return { keyPem, certPem, fingerprintSha256 };
}

export function ensureTlsCredentials(stateDir) {
  fs.mkdirSync(stateDir, { recursive: true });
  const keyPath = path.join(stateDir, 'tls-key.pem');
  const certPath = path.join(stateDir, 'tls-cert.pem');

  if (fs.existsSync(keyPath) && fs.existsSync(certPath)) {
    try {
      const keyPem = fs.readFileSync(keyPath, 'utf8');
      const certPem = fs.readFileSync(certPath, 'utf8');
      const x509 = new crypto.X509Certificate(certPem);
      const validTo = Date.parse(x509.validTo);
      if (Number.isFinite(validTo) && validTo > Date.now() + 7 * 24 * 3600 * 1000) {
        return {
          keyPem,
          certPem,
          keyPath,
          certPath,
          fingerprintSha256: x509.fingerprint256.replaceAll(':', '').toLowerCase(),
        };
      }
    } catch {
      // Regenerate below if corrupted.
    }
  }

  const generated = generateSelfSignedEcdsaCert('Winmux Remote Bridge');
  fs.writeFileSync(keyPath, generated.keyPem, { encoding: 'utf8', mode: 0o600 });
  fs.writeFileSync(certPath, generated.certPem, 'utf8');
  return { ...generated, keyPath, certPath };
}
