# C23 Commons quickstart

This is the installed-node path for publishing and using an ordinary C23
static-library package without GitHub or a central registry. Start with the
live guide; it states the currently proven target and any missing authority:

```bash
zclassic23 zcode guide
zclassic23 zcode package guide
```

The rule throughout is **verify, don't trust**. A name and semantic version
are labels. The `package_root` is exact identity. An author signature says
only who made one release statement; a provider record says only where bytes
are claimed to be available; a build receipt says only what one exact build
observed. None proves general safety, usefulness, or human acceptance.

The copyable package-store commands below use `/tmp/zclassic23-commons` so an
experiment cannot fall back to the operator's live node datadir. Keep an
isolated datadir, or deliberately replace it with the intended package-host
datadir after completing the preflight.

## One-time node preflight

The network path requires a running full node started with `-packagehost=1`.
Inspect its live state first:

```bash
zclassic23 zcode network status --datadir=/tmp/zclassic23-commons
```

If the DHT is disabled, `zcode network delegate` names the required active,
finalized ZID master input; do not pretend a local key is network admission.
The operator must also allow the package namespace under local policy. Plan
and inspect this once, commit the exact returned token, then restart so the
running DHT loads the policy:

```bash
zclassic23 zcode network policy mutate --datadir=/tmp/zclassic23-commons \
  --input='{"mode":"plan","operation":"add","source":"local","effect":"allow","scope":"service_type","action_mask":63,"value":"zclassic23.package"}'
zclassic23 zcode network policy mutate --datadir=/tmp/zclassic23-commons \
  --input='{"mode":"commit","operation":"add","source":"local","effect":"allow","scope":"service_type","action_mask":63,"value":"zclassic23.package","plan_token":"<returned token>"}'
```

## Author

Put `zcode-package.json`, `LICENSE`, public headers, C sources, and tests in a
directory outside ZClassic23. Declare every dependency by its exact
`package_root`. Discover any input spelling with `zclassic23 discover schema
<leaf>`.

1. Create a package-only key. It is not a wallet or node identity:

   ```bash
   zclassic23-package-sign --generate ./author.key
   ```

2. Derive the exact package, recipe, dependency-lock, API, and signing roots
   without writing to the node:

   ```bash
   zclassic23 zcode package dev prepare --datadir=/tmp/zclassic23-commons \
     --input='{"dir":"/absolute/path/to/package","publisher_pubkey":"<66hex>","publisher_sequence":1}'
   ```

3. Sign the returned `release_signing_digest` while keeping the private key
   off the process arguments, then seal the returned release body:

   ```bash
   exec 7<./author.key
   zclassic23-package-sign --sign-digest <64hex-digest> --key-fd 7
   exec 7<&-

   zclassic23 zcode package dev seal --datadir=/tmp/zclassic23-commons \
     --input='{"release_body_hex":"<prepare value>","signature_hex":"<128hex signature>"}'
   ```

4. Pass the sealed `release_hex` and prepare's `manifest_hex` and
   `recipe_hex` through the same command twice: inspect `mode=plan`, then
   explicitly repeat with `mode=commit`. Commit returns the exact
   `package_root` and transport `transport_root`.

   ```bash
   zclassic23 zcode create --input='{"mode":"plan","release_hex":"<hex>","manifest_hex":"<hex>","recipe_hex":"<hex>","dir":"/absolute/path/to/package","datadir":"/tmp/zclassic23-commons"}'
   zclassic23 zcode create --input='{"mode":"commit","release_hex":"<same>","manifest_hex":"<same>","recipe_hex":"<same>","dir":"/absolute/path/to/package","datadir":"/tmp/zclassic23-commons"}'
   ```

5. On the running package-hosting node, publish a POINTER binding
   `package_root` to `transport_root`, then a PROVIDER record for that
   `transport_root`. Each uses `zcode network publish` first with `mode=plan`
   and then `mode=commit` plus its returned `plan_token`. The records are
   signed availability evidence, not correctness evidence.

   ```bash
   zclassic23 zcode network publish --datadir=/tmp/zclassic23-commons \
     --input='{"mode":"plan","kind":"pointer","namespace":"zclassic23.package","semantic_root":"<package_root>","transport_root":"<transport_root>","sequence":1,"not_before":<unix>,"expiry":<unix>}'
   zclassic23 zcode network publish --datadir=/tmp/zclassic23-commons \
     --input='{"mode":"commit","kind":"pointer","namespace":"zclassic23.package","semantic_root":"<same package_root>","transport_root":"<same transport_root>","sequence":1,"not_before":<same>,"expiry":<same>,"plan_token":"<returned token>"}'

   zclassic23 zcode network publish --datadir=/tmp/zclassic23-commons \
     --input='{"mode":"plan","kind":"provider","namespace":"zclassic23.package","transport_root":"<transport_root>","sequence":1,"not_before":<unix>,"expiry":<unix>}'
   zclassic23 zcode network publish --datadir=/tmp/zclassic23-commons \
     --input='{"mode":"commit","kind":"provider","namespace":"zclassic23.package","transport_root":"<same transport_root>","sequence":1,"not_before":<same>,"expiry":<same>,"plan_token":"<returned token>"}'
   ```

## Consumer

Obtain the author's exact `package_root` through any channel. There is no
global remote name search today; `zcode package search` searches releases
already verified in the local store. This avoids turning a name index into
central technical truth.

1. Discover the signed POINTER for the exact root, then fetch its returned
   `transport_root`. Fetch is resumable and inert: it does not build, link, or
   execute downloaded code.

   ```bash
   zclassic23 zcode network records --datadir=/tmp/zclassic23-commons \
     --input='{"kind":"pointer","namespace":"zclassic23.package","semantic_root":"<package_root>","include_evidence_wires":true}'

   zclassic23 zcode package fetch --datadir=/tmp/zclassic23-commons \
     --input='{"root":"<transport_root>","namespace":"zclassic23.package","maximum_bytes":268435456}'
   ```

   Repeat the identical fetch after the asynchronous transfer completes; a
   successful import reports `reconstructed=true` and the exact
   `package_root`. Repetition resumes or reuses verified bytes.

2. Inspect the imported release and its exact dependencies:

   ```bash
   zclassic23 zcode package show --datadir=/tmp/zclassic23-commons \
     --input='{"root":"<package_root>"}'
   ```

3. Build and test only after local approval. First inspect the exact lock and
   build order, then commit the returned `plan_id`:

   ```bash
   zclassic23 zcode use --input='{"name_or_root":"<package_root>","datadir":"/tmp/zclassic23-commons"}'
   zclassic23 zcode use --input='{"plan_id":"<plan_id>","datadir":"/tmp/zclassic23-commons"}'
   ```

The installed result is public headers plus a static archive. ZClassic23 does
not load it into the node. Linking an application against it remains an
explicit local action.

## Reproducer

Repeat the consumer's exact-root `zcode use` plan and commit on a separately
installed full node. Compare the package root, dependency lock, declared
target/profile, build receipt, and every artifact root. Then inspect locally
filed observations:

```bash
zclassic23 zcode package verify --input='{"root":"<package_root>","datadir":"/tmp/zclassic23-commons"}'
```

A mismatch is evidence and must remain visible; never pick one result because
it arrived first or came from a preferred signer. Signed asynchronous worker
receipts currently bind candidate action IDs, while released-package
reproduction uses exact local build receipts. A direct signed-worker request
bound to a released `package_root` is not yet exposed, and the live guide says
so rather than claiming it exists.

## Portability

Package source identity is architecture-neutral. Build receipts deliberately
bind their target and toolchain. The currently proven package target is
`linux-x86_64-v3`; other targets must produce their own exact receipts and are
not yet claimed. ZClassic23 itself also has a wider Linux x86-64 release path
using ordinary GCC/Clang, a glibc 2.31 sysroot, and the original x86-64 CPU
baseline. Zig is not required.

The permanent clean-prefix scenario reruns this lifecycle with installed
binaries, four interchangeable full nodes, an outside-tree two-dependency
package, inert fetch, explicit builds, updates, exact revert, publisher
disappearance, and independent observations: `make
c23-commons-installed-acceptance` from a source checkout.
