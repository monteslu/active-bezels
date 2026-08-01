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

  /*
   * A universal package matches everything, by declaration.
   *
   * Checked FIRST and unconditionally: there is nothing to compare, because
   * the package makes no claim about which ROM it is for. This is a distinct
   * level from 'forced' -- forced means the user overrode a failed match,
   * universal means the author said no match was ever needed -- so a host can
   * report the difference honestly instead of implying an override happened.
   */
  if (manifest.universal) {
    return { level: 'universal', identity, rule: null, label: null };
  }

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
