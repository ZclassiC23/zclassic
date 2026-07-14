#!/usr/bin/env bash
# Fail-loud local Markdown target gate.
#
# Scans tracked Markdown as it exists in the working tree.  Relative file and
# directory links must resolve inside the repository.  Network/mail/app URIs,
# page anchors, site-absolute paths, images, fenced/inline-code examples, and
# explicit generated placeholders are intentionally outside this gate.
set -euo pipefail

SCRIPT_PATH="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

fail_selftest() {
    echo "check_markdown_links selftest: FAIL — $*" >&2
    exit 1
}

run_selftest() {
    local tmp repo empty out rc
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/zcl-markdown-links.XXXXXX")"
    SELFTEST_TMP="$tmp"
    trap 'rm -rf "${SELFTEST_TMP:-}"' EXIT HUP INT TERM
    repo="$tmp/repo"
    empty="$tmp/empty"
    mkdir -p "$repo/docs/guide" "$empty"
    git -C "$repo" init -q
    git -C "$empty" init -q

    printf '# Guide\n' > "$repo/docs/guide.md"
    printf '# Nested guide\n' > "$repo/docs/guide/index.md"
    printf '# Space\n' > "$repo/docs/with space.md"
    cat > "$repo/README.md" <<'EOF'
# Link fixture

[file](docs/guide.md)
[file fragment](docs/guide.md#heading)
[directory](docs/guide/)
[angle path](<docs/with space.md>)
[title](docs/guide.md "Guide")
[reference][guide]
[guide]: docs/guide.md
[external](https://example.invalid/missing)
[mail](mailto:nobody@example.invalid)
[app](app://fixture/missing)
[anchor](#link-fixture)
[site absolute](/docs/generated-route)
![ignored image](docs/missing-image.png)
[generated angle](docs/<generated-name>.md)
[generated env](docs/${GENERATED_NAME}.md)
[generated template](docs/{{generated_name}}.md)
[generated printf](docs/%s.md)
[generated glob](docs/*.md)
[generated ellipsis](docs/.../file.md)
`[inline example](docs/missing-inline.md)`

```markdown
[fenced example](docs/missing-fenced.md)
```

[![badge](https://example.invalid/badge.svg)](docs/guide.md)
EOF
    cat > "$repo/untracked.md" <<'EOF'
[untracked inputs are deliberately outside the gate](missing.md)
EOF
    git -C "$repo" add README.md docs/guide.md docs/guide/index.md \
        "docs/with space.md"

    if ! out="$(env -u ZCL_MARKDOWN_LINKS_SELFTEST \
        "$SCRIPT_PATH" "$repo" 2>&1)"; then
        fail_selftest "positive fixture rejected: $out"
    fi
    grep -Fq 'check_markdown_links: clean' <<<"$out" ||
        fail_selftest "positive fixture omitted clean receipt: $out"

    cp "$repo/README.md" "$tmp/readme.valid"
    printf '\n[missing target](docs/missing.md)\n' >> "$repo/README.md"
    rc=0
    out="$(env -u ZCL_MARKDOWN_LINKS_SELFTEST \
        "$SCRIPT_PATH" "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 1 ] ||
        fail_selftest "negative fixture expected exit 1, got $rc: $out"
    grep -Fq 'README.md:' <<<"$out" ||
        fail_selftest "negative fixture omitted source line: $out"
    grep -Fq 'docs/missing.md' <<<"$out" ||
        fail_selftest "negative fixture omitted missing target: $out"

    # A lexically in-repository link must not escape through a symlink. Keep
    # this distinct from the ordinary missing-target proof so both failure
    # classes remain observable.
    cp "$tmp/readme.valid" "$repo/README.md"
    printf '# Outside\n' > "$tmp/outside.md"
    ln -s ../../outside.md "$repo/docs/outbound.md"
    git -C "$repo" add docs/outbound.md
    printf '\n[outbound symlink](docs/outbound.md)\n' >> "$repo/README.md"
    rc=0
    out="$(env -u ZCL_MARKDOWN_LINKS_SELFTEST \
        "$SCRIPT_PATH" "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 1 ] ||
        fail_selftest "outbound symlink expected exit 1, got $rc: $out"
    grep -Fq 'outside repository through a symlink' <<<"$out" ||
        fail_selftest "outbound symlink omitted containment receipt: $out"

    rc=0
    out="$(env -u ZCL_MARKDOWN_LINKS_SELFTEST \
        "$SCRIPT_PATH" "$empty" 2>&1)" || rc=$?
    [ "$rc" -eq 2 ] ||
        fail_selftest "empty scan expected fail-loud exit 2, got $rc: $out"
    grep -Fq 'FATAL' <<<"$out" ||
        fail_selftest "empty scan omitted FATAL receipt: $out"

    echo "check_markdown_links selftest: PASS"
}

if [ "${1:-}" = "--self-test" ] ||
   [ "${ZCL_MARKDOWN_LINKS_SELFTEST:-0}" = "1" ]; then
    run_selftest
    exit 0
fi

ROOT="${1:-.}"
[ -d "$ROOT" ] || {
    echo "check_markdown_links: FATAL — root is not a directory: $ROOT" >&2
    exit 2
}
command -v git >/dev/null 2>&1 || {
    echo "check_markdown_links: FATAL — git is required" >&2
    exit 2
}
command -v perl >/dev/null 2>&1 || {
    echo "check_markdown_links: FATAL — perl is required" >&2
    exit 2
}
ROOT="$(cd "$ROOT" && pwd)"
git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    echo "check_markdown_links: FATAL — not a Git worktree: $ROOT" >&2
    exit 2
}

perl - "$ROOT" <<'PERL'
use strict;
use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);

my $root = shift @ARGV;
chdir $root or fatal("cannot enter repository root '$root': $!");
$root = abs_path('.') or fatal("cannot canonicalize repository root: $!");

sub fatal {
    my ($message) = @_;
    print STDERR "check_markdown_links: FATAL — $message\n";
    exit 2;
}

sub strip_inline_code {
    my ($line) = @_;
    # Markdown code spans use a matching run of backticks.  Removing them
    # prevents documentation examples from becoming filesystem contracts.
    1 while $line =~ s/(`+)[^`]*\1//g;
    return $line;
}

sub inline_targets {
    my ($line) = @_;
    my @targets;
    my $length = length $line;

    for (my $i = 0; $i < $length; $i++) {
        next unless substr($line, $i, 1) eq '[';
        next if $i > 0 && substr($line, $i - 1, 1) =~ /[!\\]/;

        my $depth = 1;
        my $close = -1;
        my $escaped = 0;
        for (my $j = $i + 1; $j < $length; $j++) {
            my $ch = substr($line, $j, 1);
            if ($escaped) {
                $escaped = 0;
                next;
            }
            if ($ch eq '\\') {
                $escaped = 1;
                next;
            }
            $depth++ if $ch eq '[';
            if ($ch eq ']') {
                $depth--;
                if ($depth == 0) {
                    $close = $j;
                    last;
                }
            }
        }
        next if $close < 0;

        # CommonMark requires the opening parenthesis immediately after the
        # closing label bracket.  Allowing whitespace here misclassifies task
        # boxes and mathematical prose such as "[h-K, h] (K = ...)".
        my $open = $close + 1;
        next unless $open < $length && substr($line, $open, 1) eq '(';

        my $pos = $open + 1;
        $pos++ while $pos < $length && substr($line, $pos, 1) =~ /\s/;
        my $target = '';
        my $end = $pos;

        if ($pos < $length && substr($line, $pos, 1) eq '<') {
            my $angle_end = index($line, '>', $pos + 1);
            next if $angle_end < 0;
            $target = substr($line, $pos + 1, $angle_end - $pos - 1);
            $end = $angle_end + 1;
        } else {
            my $paren_depth = 0;
            my $target_escaped = 0;
            for ($end = $pos; $end < $length; $end++) {
                my $ch = substr($line, $end, 1);
                if ($target_escaped) {
                    $target .= $ch;
                    $target_escaped = 0;
                    next;
                }
                if ($ch eq '\\') {
                    $target .= $ch;
                    $target_escaped = 1;
                    next;
                }
                if ($ch eq '(') {
                    $paren_depth++;
                    $target .= $ch;
                    next;
                }
                if ($ch eq ')') {
                    last if $paren_depth == 0;
                    $paren_depth--;
                    $target .= $ch;
                    next;
                }
                last if $ch =~ /\s/ && $paren_depth == 0;
                $target .= $ch;
            }
        }

        push @targets, $target if length $target;
        $i = $end > $close ? $end : $close;
    }
    return @targets;
}

sub is_generated_placeholder {
    my ($target) = @_;
    return 1 if $target =~ /\$\{[^}]*\}/;
    return 1 if $target =~ /\{\{[^}]*\}\}/;
    return 1 if $target =~ /<[^>]+>/;
    return 1 if $target =~ /(?:^|\/)\.\.\.(?:\/|$)/;
    return 1 if $target =~ /[*]/;
    return 1 if $target =~ /%[A-Za-z]/;
    return 0;
}

sub normalize_target {
    my ($source, $raw) = @_;
    my $target = $raw;
    $target =~ s/^\s+|\s+$//g;
    return (undef, 'ignored') unless length $target;
    return (undef, 'ignored') if $target =~ /^#/;
    return (undef, 'ignored') if $target =~ m{^/};
    return (undef, 'ignored') if $target =~ m{^//};
    return (undef, 'ignored') if $target =~ /^[A-Za-z][A-Za-z0-9+.-]*:/;
    return (undef, 'ignored') if is_generated_placeholder($target);

    # Fragments and query strings describe a view of an existing local target;
    # this gate intentionally verifies the filesystem target, not anchors.
    $target =~ s/[#?].*$//;
    return (undef, 'ignored') unless length $target;
    $target =~ s/%([0-9A-Fa-f]{2})/chr(hex($1))/eg;
    $target =~ s/\\([\\ ()#?])/$1/g;

    my $joined = dirname($source) . '/' . $target;
    my @parts;
    for my $part (split m{/+}, $joined) {
        next if $part eq '' || $part eq '.';
        if ($part eq '..') {
            return (undef, 'escape') unless @parts;
            pop @parts;
            next;
        }
        push @parts, $part;
    }
    return (join('/', @parts), 'local');
}

my @markdown;
open my $git, '-|', 'git', 'ls-files', '-z', '--', '*.md'
    or fatal("cannot enumerate tracked Markdown: $!");
{
    local $/ = "\0";
    while (defined(my $path = <$git>)) {
        $path =~ s/\0$//;
        push @markdown, $path if length $path;
    }
}
close $git or fatal('git ls-files failed while enumerating Markdown');

my $tracked_count = scalar @markdown;
fatal('tracked Markdown scan set is empty') if $tracked_count < 1;

my @broken;
my $scanned = 0;
my $local_targets = 0;
for my $source (@markdown) {
    # A tracked path missing from the working tree is an in-progress deletion:
    # do not parse it as a source.  Any surviving inbound link still fails.
    next unless -f $source;
    open my $fh, '<', $source
        or fatal("cannot read tracked Markdown '$source': $!");
    $scanned++;
    my $line_number = 0;
    my $fence_char = '';
    my $fence_length = 0;
    while (my $line = <$fh>) {
        $line_number++;
        if ($line =~ /^\s*(`{3,}|~{3,})/) {
            my $marker = $1;
            my $char = substr($marker, 0, 1);
            if (!$fence_char) {
                $fence_char = $char;
                $fence_length = length $marker;
            } elsif ($char eq $fence_char && length($marker) >= $fence_length) {
                $fence_char = '';
                $fence_length = 0;
            }
            next;
        }
        next if $fence_char;
        $line = strip_inline_code($line);

        my @targets = inline_targets($line);
        if ($line =~ /^\s{0,3}\[[^]]+\]:\s*(?:<([^>]+)>|(\S+))/) {
            push @targets, defined($1) ? $1 : $2;
        }
        for my $raw (@targets) {
            my ($resolved, $kind) = normalize_target($source, $raw);
            if ($kind eq 'escape') {
                push @broken,
                    [$source, $line_number, $raw, '<outside repository>',
                     'escapes the repository'];
                next;
            }
            next unless $kind eq 'local';
            $local_targets++;
            if (!-e $resolved) {
                push @broken, [$source, $line_number, $raw, $resolved,
                               'does not exist'];
                next;
            }
            my $canonical = abs_path($resolved);
            my $inside = defined($canonical) &&
                         ($canonical eq $root ||
                          index($canonical, "$root/") == 0);
            push @broken, [$source, $line_number, $raw, $resolved,
                           'resolves outside repository through a symlink']
                unless $inside;
        }
    }
    close $fh or fatal("cannot close tracked Markdown '$source': $!");
}

fatal('readable tracked Markdown scan set is empty') if $scanned < 1;
fatal('parser found zero local Markdown targets') if $local_targets < 1;

if (@broken) {
    for my $row (sort {
        $a->[0] cmp $b->[0] || $a->[1] <=> $b->[1] || $a->[2] cmp $b->[2]
    } @broken) {
        print STDERR "FAIL: $row->[0]:$row->[1] local Markdown target " .
                     "'$row->[2]' resolves to '$row->[3]' and $row->[4]\n";
    }
    print STDERR "check_markdown_links: FAIL — broken=" . scalar(@broken) .
                 " scanned=$scanned local_targets=$local_targets\n";
    exit 1;
}

print "check_markdown_links: clean — scanned=$scanned " .
      "local_targets=$local_targets\n";
exit 0;
PERL
