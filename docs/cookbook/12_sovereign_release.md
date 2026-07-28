# 12_sovereign_release — your first chain-verified release in 5 minutes

What it demonstrates: the sovereign identity layer end to end, with nothing
to install and nothing to trust but the chain you already synced. You will
verify a signed release, sign your own, and anchor an epoch — the
~40-byte transaction that commits the *entire overlay state* of the
network. Depth lives in `docs/spec/sovereign-identity-layer.md`; this page
is the fast path.

Conventions: commands are one-shot native commands
(`build/bin/zclassic23 <command>`). Every command here either works
read-only or prints exactly what to do next.

## Step 0 — verify a release (read-only, always succeeds)

A release record is a `zid_doc`: an ed25519 signature over
`name ‖ version ‖ manifest_root`, made by the publisher's chain-anchored
master key. Verifying needs no wallet, no network, no chain — just the doc:

```bash
build/bin/zclassic23 zcode release verify --file=<release.zid>
```

Expected output sketch:

```
name=demo-lib version=0.1.0
manifest_root=9f2c…(64 hex)
master_pubkey=d75a…(64 hex)
seq=1 expiry=1893456000 valid=true
```

Invalid docs are hard errors with named codes — `DOC_EXPIRED`,
`BAD_SIGNATURE`, `NOT_A_RELEASE_BODY`, `DOC_DECODE_FAILED` — so the
failure mode is always explicit. That is the whole trust model: the
signature either chains to the publisher's key or it doesn't.

## Step 1 — make your publisher key (one command, keep it secret)

Any 32-byte seed is a publisher identity. Generate one and lock it down:

```bash
head -c 32 /dev/urandom | xxd -p -c 64 > ~/.zcl-release-seed
chmod 600 ~/.zcl-release-seed
```

`zcode release sign` **refuses** a seed file with looser permissions —
your master key is your reputation; treat it like a wallet seed. It is
never logged, echoed, or written anywhere but your file.

## Step 2 — sign your first release

```bash
build/bin/zclassic23 zcode release sign \
    --name=my-lib --version=0.1.0 \
    --root=<64-hex sha3 of your release tarball> \
    --seed-file=~/.zcl-release-seed
```

Expected output: the signed doc hex, your `master_pubkey` (this is your
publisher identity — share it, pin it in your README), and the path of the
saved `<datadir>/zcode/releases/my-lib-0.1.0.zid`.

Now anyone with the doc can run Step 0 against *your* release. New
version? Increment `--seq` — consumers reject out-of-order docs, so a
replayed old release can't masquerade as the current one.

## Step 3 — anchor an epoch (optional, one tx, operator's choice)

The node's OP_RETURN catalog already commits every overlay record it has
ever seen (every ZNAM name, ZSLP transfer, ZANC anchor) into one rolling
digest. Anchoring it publishes that commitment on-chain:

```bash
build/bin/zclassic23 core epoch status     # tip, catalog digest, anchored?
build/bin/zclassic23 core epoch anchor     # with wallet: broadcasts; without:
                                           # returns op_return_hex to include
                                           # in any tx you make yourself
build/bin/zclassic23 core epoch verify     # recompute + match against chain
```

One ~40-byte OP_RETURN commits the whole overlay state. Every honest node
computes the *same* digest, so when independent operators anchor the same
epoch, the anchors cross-check each other — agreement is public
confirmation, disagreement is a named, visible problem.

## Step 4 — prove it to someone else

Hand anyone three things: the `.zid` file, your `master_pubkey`, and the
epoch anchor's txid. They verify the doc locally (Step 0), and the epoch
anchor tells them the overlay state their node derives matches the one the
network committed. No GitHub account, no CA, no keyserver, no CDN.

## Where this goes next

`docs/spec/sovereign-identity-layer.md` — release batches folded into the
`zcode` domain MMR (one anchor covers a whole day's releases), swarm
distribution of packages, FROST t-of-n publisher committees, and the
pruning story that makes "no byte is stuck forever" true.
