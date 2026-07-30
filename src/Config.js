function normalize(setting, value) {
  if (value === undefined) value = setting.default;
  switch (setting.type) {
    case 'boolean': return !!value;
    case 'integer': {
      let n = Math.round(Number(value));
      if (!Number.isFinite(n)) n = Number(setting.default ?? 0);
      return Math.max(setting.min ?? -2147483648, Math.min(setting.max ?? 2147483647, n));
    }
    case 'float':
    case 'number': {
      let n = Number(value);
      if (!Number.isFinite(n)) n = Number(setting.default ?? 0);
      return Math.max(setting.min ?? -Number.MAX_VALUE, Math.min(setting.max ?? Number.MAX_VALUE, n));
    }
    case 'choice': {
      const choices = setting.choices.map((choice) => typeof choice === 'object' ? choice.value : choice);
      return choices.includes(value) ? value : (setting.default ?? choices[0]);
    }
    case 'color': return /^#[0-9a-f]{6}(?:[0-9a-f]{2})?$/i.test(value ?? '') ? value : (setting.default ?? '#ffffff');
    case 'action': return Number(value) || 0;
    default: return value;
  }
}

export class ActiveBezelConfig {
  constructor(schema = [], values = {}) {
    this.schema = schema;
    this.values = {};
    for (const setting of schema) this.values[setting.key] = normalize(setting, values[setting.key]);
  }

  set(key, value) {
    const setting = this.schema.find((item) => item.key === key);
    if (!setting) throw new Error(`unknown Active Bezel setting: ${key}`);
    this.values[key] = setting.type === 'action'
      ? (Number(this.values[key]) || 0) + 1
      : normalize(setting, value);
    return this.values[key];
  }

  get(key) {
    return this.values[key];
  }
}
