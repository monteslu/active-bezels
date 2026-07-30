import crypto from 'node:crypto';

export function identifyRom(bytes, platform) {
  const data = Buffer.from(bytes);
  return {
    platform,
    size: data.length,
    sha256: crypto.createHash('sha256').update(data).digest('hex'),
  };
}

export function matchActiveBezel(manifest, romBytes, platform, { force = false } = {}) {
  const identity = identifyRom(romBytes, platform);
  const exact = manifest.games.find((g) =>
    g.platform === platform && g.sha256.toLowerCase() === identity.sha256);
  if (exact) return { level: 'exact', identity, rule: exact, label: exact.label ?? null };

  for (const rule of manifest.compatible) {
    if (rule.platform !== platform || rule.size !== identity.size) continue;
    const signaturesMatch = rule.signatures.every((signature) => {
      const expected = Buffer.from(signature.bytes, 'hex');
      const actual = romBytes.subarray(signature.offset, signature.offset + expected.length);
      return actual.length === expected.length && Buffer.compare(actual, expected) === 0;
    });
    if (signaturesMatch) return { level: 'compatible', identity, rule, label: rule.label ?? null };
  }
  return { level: force ? 'forced' : 'none', identity, rule: null, label: null };
}
