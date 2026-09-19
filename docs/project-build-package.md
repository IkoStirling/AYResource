# Project Build & Package

`AYProjectBuild` is the project-level authoring pipeline above the existing
`AYImportJob`, converters, `AYStorage`, and runtime `ResourceManager`.
It deliberately keeps four decisions separate:

1. **Code build** — optional CMake configure/build and one explicit artifact.
2. **Representation** — `raw`, `cook`, `auto`, or `exclude` per ordered rule.
3. **Storage** — `loose` or `pak:<chunk>`, independent of representation.
4. **Publication** — build into a sibling staging directory, then replace the
   configured output only after every prior step succeeds.

Schema version 1 always requires `package.atomic: true`. `run.workingDirectory`
is relative to the published package directory, so `"."` starts the game next
to the staged executable and `"Content"` starts it inside packaged content.

Profiles are checked-in `*.aybuild.json` files. Paths are project-relative;
rules are first-match-wins and use `*`, `?`, and `**` glob syntax.

```json
{
  "schemaVersion": 1,
  "id": "windows-development",
  "target": {
    "platform": "windows",
    "architecture": "x64",
    "configuration": "Development"
  },
  "code": {
    "enabled": true,
    "backend": "cmake",
    "configurePreset": "windows-debug",
    "buildPreset": "windows-debug",
    "target": "MyGame",
    "artifact": "out/build/windows-debug/MyGame.exe"
  },
  "content": {
    "assetRoot": "Assets",
    "outputSubdirectory": "Content",
    "defaultTransform": "raw",
    "defaultStorage": "loose",
    "rules": [
      {
        "match": "Models/**/*.fbx",
        "transform": "cook",
        "storage": "pak:core",
        "cookTextures": true
      },
      {
        "match": "UI/**/*.json",
        "transform": "raw",
        "storage": "loose"
      },
      {
        "match": "Developer/**",
        "transform": "exclude"
      }
    ]
  },
  "cache": {
    "enabled": true,
    "root": ".cookCache",
    "policy": "auto"
  },
  "package": {
    "output": "out/package/windows-development",
    "compression": "zstd",
    "atomic": true
  },
  "run": {
    "workingDirectory": ".",
    "arguments": ["-asset-root", "Content"]
  }
}
```

## Cook cache

Cook objects live under `.cookCache/objects/<prefix>/<sha256>`. The key covers
source bytes, target identity, logical path, converter options, and the FBX
importer contract. FBX/glTF sibling files are conservatively included in the
source closure because those formats can reference external images without
listing source paths in the cooked dependency sidecar. This may create an
extra miss but cannot return stale output.

Objects are immutable and are first written below `.cookCache/staging`, then
renamed into the object store. `auto` reuses hits and cooks misses;
`cacheOnly`/`--no-cook` rejects misses; `force` replaces the matching object.

## Runtime boundary

Raw data may be loose or stored in a Pak. A raw file with a registered runtime
loader receives a `resources.db` record. Opaque files can still be included,
but a Pak-stored opaque file has no `ResourceManager` loader and produces a
warning; keep UI/Flow/Scene JSON loose until their consumers use a package VFS.

Successful code builds write `.ayeditor/builds/last-success.json`. AYEditor
Project Run resolves an explicit `.ayeditor/run.json` first, then this exact
build state, then the shared project descriptor and conventional fallbacks.

## CLI

```bat
project_build_tool --project D:\Games\MyGame ^
  --profile BuildProfiles\windows-development.aybuild.json
```

Use `--plan` for a deterministic JSON plan, `--dry-run` for validation and
counts, `--skip-code` for content-only builds, and `--skip-package` to stage
Pak-selected content as loose files. When code build is enabled,
`--skip-code` skips CMake but still requires and stages the configured artifact;
this supports fast content-only repackaging of an existing executable.
