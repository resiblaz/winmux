// Pure zero-dependency ISO/IEC 18004 QR Code encoder (Byte mode, ECC Level L, Versions 1..15).
// Supports UTF-8 payloads up to 520 bytes (winmux://pair URIs are typically 150-240 bytes).

const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);
(() => {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    GF_EXP[i] = x;
    GF_LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) {
    GF_EXP[i] = GF_EXP[i - 255];
  }
})();

function gfMul(a, b) {
  if (a === 0 || b === 0) return 0;
  return GF_EXP[GF_LOG[a] + GF_LOG[b]];
}

function rsGeneratorPoly(degree) {
  let poly = [1];
  for (let i = 0; i < degree; i++) {
    const next = new Array(poly.length + 1).fill(0);
    const factor = GF_EXP[i];
    for (let j = 0; j < poly.length; j++) {
      next[j] ^= poly[j];
      next[j + 1] ^= gfMul(poly[j], factor);
    }
    poly = next;
  }
  return poly;
}

function rsRemainder(data, ecLen) {
  const gen = rsGeneratorPoly(ecLen);
  const res = new Uint8Array(ecLen);
  for (let i = 0; i < data.length; i++) {
    const factor = data[i] ^ res[0];
    res.copyWithin(0, 1);
    res[ecLen - 1] = 0;
    for (let j = 0; j < ecLen; j++) {
      res[j] ^= gfMul(gen[j + 1], factor);
    }
  }
  return res;
}

// Version 1..15 Level L specification: [totalDataCodewords, ecPerBlock, g1Blocks, g1Data, g2Blocks, g2Data]
const VERSION_L_TABLE = [
  null,
  [19, 7, 1, 19, 0, 0],     // V1
  [34, 10, 1, 34, 0, 0],    // V2
  [55, 15, 1, 55, 0, 0],    // V3
  [80, 20, 1, 80, 0, 0],    // V4
  [108, 26, 1, 108, 0, 0],  // V5
  [136, 18, 2, 68, 0, 0],   // V6
  [156, 20, 2, 78, 0, 0],   // V7
  [194, 24, 2, 97, 0, 0],   // V8
  [232, 30, 2, 116, 0, 0],  // V9
  [274, 18, 2, 68, 2, 69],  // V10
  [324, 20, 4, 81, 0, 0],   // V11
  [370, 24, 2, 92, 2, 93],  // V12
  [428, 26, 4, 107, 0, 0],  // V13
  [461, 30, 3, 115, 1, 116], // V14
  [523, 22, 5, 87, 1, 88],  // V15
];

const ALIGNMENT_POSITIONS = [
  null,
  [],
  [6, 18],
  [6, 22],
  [6, 26],
  [6, 30],
  [6, 34],
  [6, 22, 38],
  [6, 24, 42],
  [6, 26, 46],
  [6, 28, 50],
  [6, 30, 54],
  [6, 32, 58],
  [6, 34, 62],
  [6, 26, 46, 66],
  [6, 26, 48, 70],
];

function selectVersion(byteLen) {
  for (let v = 1; v < VERSION_L_TABLE.length; v++) {
    const capacity = VERSION_L_TABLE[v][0];
    const headerBits = 4 + (v <= 9 ? 8 : 16);
    const maxBytes = Math.floor((capacity * 8 - headerBits) / 8);
    if (byteLen <= maxBytes) return v;
  }
  throw new Error(`QR payload too large (${byteLen} bytes)`);
}

function buildCodewords(bytes, version) {
  const [totalData, ecPerBlock, g1Blocks, g1Data, g2Blocks, g2Data] = VERSION_L_TABLE[version];
  const bits = [];
  const pushBits = (val, len) => {
    for (let i = len - 1; i >= 0; i--) bits.push((val >>> i) & 1);
  };

  // Byte mode indicator: 0100
  pushBits(0b0100, 4);
  pushBits(bytes.length, version <= 9 ? 8 : 16);
  for (const b of bytes) pushBits(b, 8);

  const totalBits = totalData * 8;
  // Terminator up to 4 zero bits
  for (let i = 0; i < 4 && bits.length < totalBits; i++) bits.push(0);
  // Pad to byte boundary
  while (bits.length % 8 !== 0) bits.push(0);

  const data = new Uint8Array(totalData);
  const byteCount = bits.length / 8;
  for (let i = 0; i < byteCount; i++) {
    let v = 0;
    for (let b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
    data[i] = v;
  }
  // Pad bytes alternating 0xEC, 0x11
  const padBytes = [0xec, 0x11];
  for (let i = byteCount, p = 0; i < totalData; i++, p ^= 1) {
    data[i] = padBytes[p];
  }

  // Split into RS blocks
  const dataBlocks = [];
  const ecBlocks = [];
  let offset = 0;
  for (let i = 0; i < g1Blocks; i++) {
    const blk = data.subarray(offset, offset + g1Data);
    offset += g1Data;
    dataBlocks.push(blk);
    ecBlocks.push(rsRemainder(blk, ecPerBlock));
  }
  for (let i = 0; i < g2Blocks; i++) {
    const blk = data.subarray(offset, offset + g2Data);
    offset += g2Data;
    dataBlocks.push(blk);
    ecBlocks.push(rsRemainder(blk, ecPerBlock));
  }

  // Interleave data codewords then EC codewords
  const out = [];
  const maxDataLen = Math.max(g1Data, g2Data);
  for (let i = 0; i < maxDataLen; i++) {
    for (const blk of dataBlocks) {
      if (i < blk.length) out.push(blk[i]);
    }
  }
  for (let i = 0; i < ecPerBlock; i++) {
    for (const ec of ecBlocks) {
      out.push(ec[i]);
    }
  }
  return Uint8Array.from(out);
}

function bchFormatBits(mask) {
  // ECC Level L = 01
  const data = (0b01 << 3) | mask;
  let rem = data << 10;
  for (let i = 14; i >= 10; i--) {
    if ((rem >>> i) & 1) rem ^= 0x537 << (i - 10);
  }
  return ((data << 10) | rem) ^ 0x5412;
}

function bchVersionBits(version) {
  let rem = version << 12;
  for (let i = 17; i >= 12; i--) {
    if ((rem >>> i) & 1) rem ^= 0x1f25 << (i - 12);
  }
  return (version << 12) | rem;
}

function createBaseMatrix(version) {
  const size = version * 4 + 17;
  const grid = Array.from({ length: size }, () => new Uint8Array(size));
  const reserved = Array.from({ length: size }, () => new Uint8Array(size));

  const setFunc = (r, c, dark) => {
    if (r < 0 || c < 0 || r >= size || c >= size) return;
    grid[r][c] = dark ? 1 : 0;
    reserved[r][c] = 1;
  };

  // Finder patterns + separators
  const placeFinder = (top, left) => {
    for (let dr = -1; dr <= 7; dr++) {
      for (let dc = -1; dc <= 7; dc++) {
        const r = top + dr;
        const c = left + dc;
        if (r < 0 || c < 0 || r >= size || c >= size) continue;
        const inOuter = dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6;
        const dark =
          inOuter &&
          (dr === 0 || dr === 6 || dc === 0 || dc === 6 || (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4));
        setFunc(r, c, dark);
      }
    }
  };
  placeFinder(0, 0);
  placeFinder(0, size - 7);
  placeFinder(size - 7, 0);

  // Alignment patterns
  const alignCoords = ALIGNMENT_POSITIONS[version];
  for (const ar of alignCoords) {
    for (const ac of alignCoords) {
      if (reserved[ar][ac]) continue;
      for (let dr = -2; dr <= 2; dr++) {
        for (let dc = -2; dc <= 2; dc++) {
          const dark = Math.max(Math.abs(dr), Math.abs(dc)) !== 1;
          setFunc(ar + dr, ac + dc, dark);
        }
      }
    }
  }

  // Timing patterns
  for (let i = 8; i < size - 8; i++) {
    const dark = i % 2 === 0;
    if (!reserved[6][i]) setFunc(6, i, dark);
    if (!reserved[i][6]) setFunc(i, 6, dark);
  }

  // Dark module
  setFunc(size - 8, 8, true);

  // Reserve format info strips
  for (let i = 0; i <= 8; i++) {
    if (i !== 6) {
      reserved[8][i] = 1;
      reserved[i][8] = 1;
    }
  }
  for (let i = size - 8; i < size; i++) {
    reserved[8][i] = 1;
    reserved[i][8] = 1;
  }

  // Version info for V >= 7
  if (version >= 7) {
    const vBits = bchVersionBits(version);
    for (let i = 0; i < 18; i++) {
      const bit = ((vBits >>> i) & 1) === 1;
      const a = Math.floor(i / 3);
      const b = size - 11 + (i % 3);
      setFunc(a, b, bit);
      setFunc(b, a, bit);
    }
  }

  return { size, grid, reserved };
}

function placeDataBits(base, codewords) {
  const { size, grid, reserved } = base;
  let bitIndex = 0;
  const totalBits = codewords.length * 8;
  let upward = true;

  for (let right = size - 1; right >= 1; right -= 2) {
    if (right === 6) right = 5; // skip vertical timing column
    for (let step = 0; step < size; step++) {
      const r = upward ? size - 1 - step : step;
      for (let colOff = 0; colOff < 2; colOff++) {
        const c = right - colOff;
        if (reserved[r][c]) continue;
        let bit = 0;
        if (bitIndex < totalBits) {
          bit = (codewords[bitIndex >>> 3] >>> (7 - (bitIndex & 7))) & 1;
          bitIndex++;
        }
        grid[r][c] = bit;
      }
    }
    upward = !upward;
  }
}

function maskBit(mask, r, c) {
  switch (mask) {
    case 0: return (r + c) % 2 === 0;
    case 1: return r % 2 === 0;
    case 2: return c % 3 === 0;
    case 3: return (r + c) % 3 === 0;
    case 4: return (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0;
    case 5: return ((r * c) % 2) + ((r * c) % 3) === 0;
    case 6: return (((r * c) % 2) + ((r * c) % 3)) % 2 === 0;
    case 7: return (((r + c) % 2) + ((r * c) % 3)) % 2 === 0;
    default: return false;
  }
}

function applyFormatBits(grid, size, mask) {
  const bits = bchFormatBits(mask);
  const getBit = (i) => (bits >>> i) & 1;
  // Top-left around finder
  for (let i = 0; i <= 5; i++) grid[8][i] = getBit(14 - i);
  grid[8][7] = getBit(8);
  grid[8][8] = getBit(7);
  grid[7][8] = getBit(6);
  for (let i = 0; i <= 5; i++) grid[5 - i][8] = getBit(5 - i);
  // Bottom-left and top-right
  for (let i = 0; i <= 6; i++) grid[size - 1 - i][8] = getBit(14 - i);
  for (let i = 0; i <= 7; i++) grid[8][size - 8 + i] = getBit(7 - i);
}

function evaluatePenalty(grid, size) {
  let penalty = 0;
  // Rule 1: adjacent modules in row/col of same color
  for (let r = 0; r < size; r++) {
    let runColor = grid[r][0];
    let runLen = 1;
    for (let c = 1; c < size; c++) {
      if (grid[r][c] === runColor) {
        runLen++;
      } else {
        if (runLen >= 5) penalty += 3 + (runLen - 5);
        runColor = grid[r][c];
        runLen = 1;
      }
    }
    if (runLen >= 5) penalty += 3 + (runLen - 5);
  }
  for (let c = 0; c < size; c++) {
    let runColor = grid[0][c];
    let runLen = 1;
    for (let r = 1; r < size; r++) {
      if (grid[r][c] === runColor) {
        runLen++;
      } else {
        if (runLen >= 5) penalty += 3 + (runLen - 5);
        runColor = grid[r][c];
        runLen = 1;
      }
    }
    if (runLen >= 5) penalty += 3 + (runLen - 5);
  }
  // Rule 2: 2x2 blocks of same color
  for (let r = 0; r < size - 1; r++) {
    for (let c = 0; c < size - 1; c++) {
      const v = grid[r][c];
      if (v === grid[r][c + 1] && v === grid[r + 1][c] && v === grid[r + 1][c + 1]) {
        penalty += 3;
      }
    }
  }
  // Rule 4: dark module balance
  let dark = 0;
  for (let r = 0; r < size; r++) {
    for (let c = 0; c < size; c++) dark += grid[r][c];
  }
  const ratio = Math.abs((dark * 100) / (size * size) - 50);
  penalty += Math.floor(ratio / 5) * 10;
  return penalty;
}

export function encodeQrMatrix(text) {
  const bytes = Buffer.from(String(text || ''), 'utf8');
  const version = selectVersion(bytes.length);
  const codewords = buildCodewords(bytes, version);
  const base = createBaseMatrix(version);
  placeDataBits(base, codewords);

  let bestMask = 0;
  let bestPenalty = Infinity;
  let bestGrid = null;

  for (let mask = 0; mask < 8; mask++) {
    const candidate = base.grid.map((row) => Uint8Array.from(row));
    for (let r = 0; r < base.size; r++) {
      for (let c = 0; c < base.size; c++) {
        if (!base.reserved[r][c] && maskBit(mask, r, c)) {
          candidate[r][c] ^= 1;
        }
      }
    }
    applyFormatBits(candidate, base.size, mask);
    const penalty = evaluatePenalty(candidate, base.size);
    if (penalty < bestPenalty) {
      bestPenalty = penalty;
      bestMask = mask;
      bestGrid = candidate;
    }
  }

  let modules = '';
  for (let r = 0; r < base.size; r++) {
    for (let c = 0; c < base.size; c++) {
      modules += bestGrid[r][c] ? '1' : '0';
    }
  }

  return {
    version,
    mask: bestMask,
    size: base.size,
    modules,
  };
}
