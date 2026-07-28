# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# telemetry_scan_lib.awk — slice one C function out of a file and print every
# scalar JSON field emission inside it as "<line> <objvar> <field>".
#
# Invoked as: awk -v fn=<function-name> -f telemetry_scan_lib.awk <file.c>
#
# A definition line starts at column 0, contains "<fn>(", and does not end in
# ';' (that would be a prototype). Call sites are always indented, so they
# never match. The slice ends at the first line that is exactly "}".
#
# Exit 3 (via the END rule) when the function was not found at all — the
# caller MUST treat that as fatal, never as "no fields".

BEGIN { inf = 0; found = 0 }

!inf && /^[A-Za-z_]/ && index($0, fn "(") > 0 && $0 !~ /;[[:space:]]*$/ {
    inf = 1
    found = 1
}

inf {
    line = $0
    while (match(line,
          /json_push_kv_(int|str|bool|dbl|uint)\([[:space:]]*&?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*,[[:space:]]*"[A-Za-z0-9_]+"/)) {
        call = substr(line, RSTART, RLENGTH)
        line = substr(line, RSTART + RLENGTH)
        # objvar = text between '(' and the first ','
        p = index(call, "(")
        rest = substr(call, p + 1)
        c = index(rest, ",")
        obj = substr(rest, 1, c - 1)
        gsub(/[[:space:]]/, "", obj)
        # field = the quoted literal
        q1 = index(rest, "\"")
        tail = substr(rest, q1 + 1)
        q2 = index(tail, "\"")
        field = substr(tail, 1, q2 - 1)
        printf "%d %s %s\n", NR, obj, field
    }
    if ($0 == "}") inf = 0
}

END { if (!found) exit 3 }
