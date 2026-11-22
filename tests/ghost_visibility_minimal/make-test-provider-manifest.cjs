const fs = require('node:fs');
const path = require('node:path');

const [outputPath] = process.argv.slice(2);
if (!outputPath) {
  throw new Error('usage: node make-test-provider-manifest.cjs <output-pplugin>');
}

const sourcePath = path.resolve(__dirname, '../../../bindings/s2sdk.pplugin');
const manifest = JSON.parse(fs.readFileSync(sourcePath, 'utf8'));
const pinPath = path.resolve(__dirname, '../../../runtime/s2sdk.json');
const pin = JSON.parse(fs.readFileSync(pinPath, 'utf8'));
if (manifest.name !== 's2sdk' || manifest.version !== pin.version) {
  throw new Error(`expected the selected S2SDK ${pin.version} provider`);
}
if (!Array.isArray(manifest.methods) || !Array.isArray(manifest.prototypes)) {
  throw new Error('selected S2SDK provider must define methods and prototypes');
}
const names = new Set(manifest.methods.map((method) => method.funcName));
if (names.size !== manifest.methods.length) {
  throw new Error('selected S2SDK provider has duplicate method imports');
}
for (const name of [
  'EmitSoundToClient', 'StopSound',
  'GetHiddenTransmitEntities', 'HideTransmitEntity', 'ShowTransmitEntity',
]) {
  if (!names.has(name)) throw new Error(`selected S2SDK provider is missing ${name}`);
}
const callback = {
  name: 'OnGameFrameCallback',
  funcName: 'OnGameFrameCallback',
  description: 'Called by S2SDK after the server GameFrame callback.',
  paramTypes: [
    { name: 'simulating', type: 'bool', description: 'Whether the server is simulating.' },
    { name: 'firstTick', type: 'bool', description: 'Whether this is the first tick.' },
    { name: 'lastTick', type: 'bool', description: 'Whether this is the last tick.' },
  ],
  retType: { type: 'void', description: '' },
};
const makeMethod = (funcName) => ({
  name: funcName,
  group: 'Listeners',
  description: funcName.endsWith('_Register')
    ? 'Register the diagnostic callback for the server GameFrame.'
    : 'Unregister the diagnostic callback for the server GameFrame.',
  funcName,
  paramTypes: [{ name: 'callback', type: 'function', ref: false, description: 'Function callback.', prototype: callback.name }],
  retType: { type: 'void' },
});

const expectedCallbackParameters = callback.paramTypes.map(({ type }) => type);
const existingCallback = manifest.prototypes.filter((prototype) => prototype.name === callback.name);
if (existingCallback.length > 1) throw new Error(`selected provider has duplicate ${callback.name} prototypes`);
if (existingCallback.length === 0) {
  manifest.prototypes.push(callback);
} else if (
  existingCallback[0].paramTypes.map(({ type }) => type).join(',') !== expectedCallbackParameters.join(',') ||
  existingCallback[0].retType?.type !== 'void'
) {
  throw new Error(`selected provider has an incompatible ${callback.name} prototype`);
}

for (const funcName of ['OnGameFrame_Register', 'OnGameFrame_Unregister']) {
  const existing = manifest.methods.filter((method) => method.funcName === funcName);
  if (existing.length > 1) throw new Error(`selected provider has duplicate ${funcName} imports`);
  if (existing.length === 0) {
    manifest.methods.push(makeMethod(funcName));
    continue;
  }
  const method = existing[0];
  if (
    method.paramTypes?.length !== 1 ||
    method.paramTypes[0].type !== 'function' ||
    method.paramTypes[0].prototype !== callback.name ||
    method.retType?.type !== 'void'
  ) {
    throw new Error(`selected provider has an incompatible ${funcName} import`);
  }
}

const destination = path.resolve(outputPath);
fs.mkdirSync(path.dirname(destination), { recursive: true });
fs.writeFileSync(destination, `${JSON.stringify(manifest, null, 2)}\n`);
process.stdout.write(`Wrote test provider: ${destination} (${manifest.methods.length} methods, ${manifest.prototypes.length} prototypes)\n`);
