/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Names (ZNAM) HTML site views. See views/name_view.h for the contract.
 *
 * Rendering only — the controller (app/controllers/src/name_site_controller.c)
 * owns routing, CSRF, the PoW gate, and the register tx-compose. The in-browser
 * proof-of-work solver below mirrors the store's (app/views/src/store_view.c):
 * SHA3-256(peer_id || ts_LE64 || nonce_LE64) must have FAST_SYNC_POW_BITS
 * leading zero bits — the exact predicate fast_sync_verify_pow() checks
 * server-side, so a solution found here verifies there. */

#include "views/name_view.h"
#include "controllers/name_controller.h"   /* znam_type_name */
#include "net/fast_sync.h"                  /* FAST_SYNC_POW_BITS */
#include "util/template.h"                  /* html_escape */

#include <stdio.h>
#include <string.h>

/* The two-part in-browser SHA3-256 PoW solver. Split into two string
 * literals to stay under ISO C99's 4095-char minimum translation limit
 * (-Woverlength-strings under -Werror); concatenated at point of use via
 * adjacent %s substitutions. Part 1 is a from-scratch keccak-f[1600] +
 * sha3_256; part 2 is the chunked nonce search + form wiring. Identical
 * algorithm to store_view.c's solver — the form id / solver symbol are
 * names-specific so the two never collide if both ever load together. */
static const char NAME_REG_POW_JS_1[] =
    "(function(){\n"
    "'use strict';\n"
    "var RHO=[0,1,62,28,27,36,44,6,55,20,3,10,43,25,39,41,45,15,21,8,18,2,61,56,14];\n"
    "var PI=[0,10,20,5,15,16,1,11,21,6,7,17,2,12,22,23,8,18,3,13,14,24,9,19,4];\n"
    "var RCLO=[0x1,0x8082,0x808a,0x80008000,0x808b,0x80000001,0x80008081,0x8009,0x8a,0x88,0x80008009,0x8000000a,0x8000808b,0x8b,0x8089,0x8003,0x8002,0x80,0x800a,0x8000000a,0x80008081,0x8080,0x80000001,0x80008008];\n"
    "var RCHI=[0,0,0x80000000,0x80000000,0,0,0x80000000,0x80000000,0,0,0,0,0,0x80000000,0x80000000,0x80000000,0x80000000,0x80000000,0,0x80000000,0x80000000,0x80000000,0,0x80000000];\n"
    "function rotl(lo,hi,n){\n"
    "  if(n===0) return [lo,hi];\n"
    "  if(n<32) return [((lo<<n)|(hi>>>(32-n)))>>>0,((hi<<n)|(lo>>>(32-n)))>>>0];\n"
    "  if(n===32) return [hi,lo];\n"
    "  var m=n-32;\n"
    "  return [((hi<<m)|(lo>>>(32-m)))>>>0,((lo<<m)|(hi>>>(32-m)))>>>0];\n"
    "}\n"
    "function keccakF1600(lo,hi){\n"
    "  var Clo=new Uint32Array(5),Chi=new Uint32Array(5),Dlo=new Uint32Array(5),Dhi=new Uint32Array(5);\n"
    "  var Blo=new Uint32Array(25),Bhi=new Uint32Array(25);\n"
    "  for(var round=0;round<24;round++){\n"
    "    for(var x=0;x<5;x++){\n"
    "      Clo[x]=lo[x]^lo[x+5]^lo[x+10]^lo[x+15]^lo[x+20];\n"
    "      Chi[x]=hi[x]^hi[x+5]^hi[x+10]^hi[x+15]^hi[x+20];\n"
    "    }\n"
    "    for(x=0;x<5;x++){\n"
    "      var r=rotl(Clo[(x+1)%5],Chi[(x+1)%5],1);\n"
    "      Dlo[x]=Clo[(x+4)%5]^r[0];\n"
    "      Dhi[x]=Chi[(x+4)%5]^r[1];\n"
    "    }\n"
    "    var i;\n"
    "    for(i=0;i<25;i++){ lo[i]^=Dlo[i%5]; hi[i]^=Dhi[i%5]; }\n"
    "    for(i=0;i<25;i++){\n"
    "      var rr=rotl(lo[i],hi[i],RHO[i]);\n"
    "      Blo[PI[i]]=rr[0]; Bhi[PI[i]]=rr[1];\n"
    "    }\n"
    "    for(var y=0;y<5;y++){\n"
    "      var base=5*y;\n"
    "      for(x=0;x<5;x++){\n"
    "        lo[base+x]=Blo[base+x]^((~Blo[base+(x+1)%5])&Blo[base+(x+2)%5]);\n"
    "        hi[base+x]=Bhi[base+x]^((~Bhi[base+(x+1)%5])&Bhi[base+(x+2)%5]);\n"
    "      }\n"
    "    }\n"
    "    lo[0]^=RCLO[round]; hi[0]^=RCHI[round];\n"
    "  }\n"
    "}\n"
    "var RATE=136;\n"
    "function sha3_256(bytes){\n"
    "  var lo=new Uint32Array(25),hi=new Uint32Array(25);\n"
    "  var padLen=RATE-(bytes.length%RATE);\n"
    "  var full=new Uint8Array(bytes.length+padLen);\n"
    "  full.set(bytes,0);\n"
    "  full[bytes.length]=0x06;\n"
    "  full[full.length-1]|=0x80;\n"
    "  for(var off=0;off<full.length;off+=RATE){\n"
    "    var i;\n"
    "    for(i=0;i<RATE/8;i++){\n"
    "      var o=off+i*8;\n"
    "      lo[i]^=(full[o]|(full[o+1]<<8)|(full[o+2]<<16)|(full[o+3]<<24))>>>0;\n"
    "      hi[i]^=(full[o+4]|(full[o+5]<<8)|(full[o+6]<<16)|(full[o+7]<<24))>>>0;\n"
    "    }\n"
    "    keccakF1600(lo,hi);\n"
    "  }\n"
    "  var out=new Uint8Array(32);\n"
    "  for(var j=0;j<4;j++){\n"
    "    var oo=j*8;\n"
    "    out[oo]=lo[j]&0xff; out[oo+1]=(lo[j]>>>8)&0xff; out[oo+2]=(lo[j]>>>16)&0xff; out[oo+3]=(lo[j]>>>24)&0xff;\n"
    "    out[oo+4]=hi[j]&0xff; out[oo+5]=(hi[j]>>>8)&0xff; out[oo+6]=(hi[j]>>>16)&0xff; out[oo+7]=(hi[j]>>>24)&0xff;\n"
    "  }\n"
    "  return out;\n"
    "}\n";

static const char NAME_REG_POW_JS_2[] =
    "function hexToBytes(hex){\n"
    "  var out=new Uint8Array(hex.length/2);\n"
    "  for(var i=0;i<out.length;i++) out[i]=parseInt(hex.substr(i*2,2),16);\n"
    "  return out;\n"
    "}\n"
    "function numToBytesLE(num,n){\n"
    "  var out=new Uint8Array(n);\n"
    "  for(var i=0;i<n;i++){ out[i]=num%256; num=Math.floor(num/256); }\n"
    "  return out;\n"
    "}\n"
    "function leadingZeroBitsOk(hash,bits){\n"
    "  var fullBytes=Math.floor(bits/8);\n"
    "  for(var i=0;i<fullBytes;i++) if(hash[i]!==0) return false;\n"
    "  var rem=bits%8;\n"
    "  if(rem>0){\n"
    "    var mask=(0xff<<(8-rem))&0xff;\n"
    "    if(hash[fullBytes]&mask) return false;\n"
    "  }\n"
    "  return true;\n"
    "}\n"
    "function strBytes(s){\n"
    "  var o=new Uint8Array(s.length);\n"
    "  for(var i=0;i<s.length;i++) o[i]=s.charCodeAt(i)&0xff;\n"
    "  return o;\n"
    "}\n"
    "function namePowSolveChunked(nameVal,ts,bits,statusEl,onDone){\n"
    "  var peer=sha3_256(strBytes('znam:register:pow:'+nameVal));\n"
    "  var tsBytes=numToBytesLE(ts,8);\n"
    "  var buf=new Uint8Array(48);\n"
    "  buf.set(peer,0);\n"
    "  buf.set(tsBytes,32);\n"
    "  var nonce=0;\n"
    "  var startTime=Date.now();\n"
    "  function step(){\n"
    "    var chunkEnd=nonce+20000;\n"
    "    for(;nonce<chunkEnd;nonce++){\n"
    "      var nb=numToBytesLE(nonce,8);\n"
    "      buf.set(nb,40);\n"
    "      var h=sha3_256(buf);\n"
    "      if(leadingZeroBitsOk(h,bits)){\n"
    "        onDone(nonce);\n"
    "        return;\n"
    "      }\n"
    "    }\n"
    "    if(statusEl) statusEl.textContent='Solving proof-of-work... '+nonce+' tries, '+((Date.now()-startTime)/1000).toFixed(1)+'s';\n"
    "    setTimeout(step,0);\n"
    "  }\n"
    "  step();\n"
    "}\n"
    "document.addEventListener('DOMContentLoaded',function(){\n"
    "  var form=document.getElementById('nameRegForm');\n"
    "  if(!form) return;\n"
    "  form.addEventListener('submit',function(ev){\n"
    "    var nonceField=document.getElementById('pow_nonce_field');\n"
    "    if(nonceField.value) return;\n"
    "    ev.preventDefault();\n"
    "    var btn=document.getElementById('regBtn');\n"
    "    var status=document.getElementById('powStatus');\n"
    "    if(btn) btn.disabled=true;\n"
    "    var nameVal=(form.elements['name'].value||'').trim();\n"
    "    var ts=parseInt(form.getAttribute('data-pow-ts'),10);\n"
    "    var bits=parseInt(form.getAttribute('data-pow-bits'),10);\n"
    "    namePowSolveChunked(nameVal,ts,bits,status,function(nonce){\n"
    "      nonceField.value=String(nonce);\n"
    "      if(status) status.textContent='Proof-of-work solved ('+nonce+' tries). Submitting...';\n"
    "      form.submit();\n"
    "    });\n"
    "  });\n"
    "});\n"
    "})();\n";

/* ── HTTP wrappers ──────────────────────────────────────────────── */

static size_t name_wrap_response(const char *body, size_t body_len,
                                 const char *status, uint8_t *resp, size_t max)
{
    return (size_t)snprintf((char *)resp, max,
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%.*s",
        status, body_len, (int)body_len, body);
}

size_t name_html_response(const char *body, size_t body_len,
                          uint8_t *resp, size_t max)
{
    return name_wrap_response(body, body_len, "200 OK", resp, max);
}

size_t name_error_response(const char *status_code,
                           const char *body, size_t body_len,
                           uint8_t *resp, size_t max)
{
    return name_wrap_response(body, body_len, status_code, resp, max);
}

/* ── Page shell ─────────────────────────────────────────────────── */

static int name_body_start(char *buf, size_t max, const char *title)
{
    return snprintf(buf, max,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title><style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88}h2{color:#00cc66}"
        "a{color:#00aaff;text-decoration:none}a:hover{color:#00ff88}"
        ".header-nav{display:flex;align-items:center;gap:16px;"
        "border-bottom:1px solid #333;padding-bottom:12px;margin-bottom:16px;"
        "flex-wrap:wrap}.header-nav a{font-size:13px}"
        ".card{background:#1a1a1a;padding:16px;margin:12px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        ".name{color:#00ff88;font-size:20px;font-weight:bold}"
        ".kv{display:flex;gap:8px;margin:4px 0;font-size:13px}"
        ".kv b{color:#888;min-width:120px;display:inline-block}"
        ".val{word-break:break-all}"
        "input{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:8px;font-family:monospace;width:100%%;margin:5px 0;"
        "box-sizing:border-box}select{background:#1a1a1a;color:#e0e0e0;"
        "border:1px solid #333;padding:8px;font-family:monospace}"
        ".btn{display:inline-block;background:#00ff88;color:#0a0a0a;"
        "padding:10px 20px;border-radius:4px;font-weight:bold;margin-top:10px;"
        "border:none;cursor:pointer;font-family:monospace}"
        "</style></head><body>"
        "<div class='header-nav'>"
        "<h1 style='margin:0'><a href='/names'>ZCL Names</a></h1>"
        "<a href='/'>Home</a>"
        "<a href='/names'>Browse</a>"
        "<a href='/names/register'>Register</a>"
        "</div>",
        title);
}

/* ── Index ──────────────────────────────────────────────────────── */

size_t name_view_index(const struct znam_entry *entries, int count,
                       uint8_t *resp, size_t max)
{
    char body[16384];
    size_t off = 0;
    int n = name_body_start(body, sizeof(body), "ZCL Names");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<p>%d registered name%s. A name is a sovereign identity for the "
        "sites this node hosts over onion + HTTPS. Visit "
        "<code>/n/&lt;name&gt;</code> to resolve one.</p>",
        count, count == 1 ? "" : "s");
    if (n > 0) off += (size_t)n;

    for (int i = 0; i < count && off < sizeof(body) - 512; i++) {
        char safe_name[128], safe_owner[128], safe_val[280];
        html_escape(safe_name, sizeof(safe_name), entries[i].name);
        html_escape(safe_owner, sizeof(safe_owner), entries[i].owner_address);
        html_escape(safe_val, sizeof(safe_val), entries[i].target_value);
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='card'>"
            "<div class='name'><a href='/names/%s'>%s</a></div>"
            "<div class='kv'><b>%s</b><span class='val'>%s</span></div>"
            "<div class='kv'><b>owner</b><span class='val'>%s</span></div>"
            "<div class='kv'><b>registered</b><span class='val'>h=%d</span></div>"
            "<a href='/n/%s'>open site &rarr;</a>"
            "</div>",
            safe_name, safe_name,
            znam_type_name(entries[i].target_type), safe_val,
            safe_owner, entries[i].reg_height, safe_name);
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off, "</body></html>");
    if (n > 0) off += (size_t)n;
    return name_html_response(body, off, resp, max);
}

/* ── Profile / default site ─────────────────────────────────────── */

size_t name_view_profile(const struct znam_entry *e,
                         const struct znam_text_record *text, int ntext,
                         const struct znam_addr_record *addr, int naddr,
                         uint8_t *resp, size_t max)
{
    char body[16384];
    size_t off = 0;
    char safe_name[128];
    html_escape(safe_name, sizeof(safe_name), e->name);

    int n = name_body_start(body, sizeof(body), safe_name);
    if (n > 0) off = (size_t)n;

    char safe_owner[128], safe_val[280];
    html_escape(safe_owner, sizeof(safe_owner), e->owner_address);
    html_escape(safe_val, sizeof(safe_val), e->target_value);

    char expires[32];
    if (e->expiry_height > 0)
        snprintf(expires, sizeof(expires), "h=%d", e->expiry_height);
    else
        snprintf(expires, sizeof(expires), "never");

    n = snprintf(body + off, sizeof(body) - off,
        "<div class='card'>"
        "<div class='name'>%s</div>"
        "<div class='kv'><b>primary %s</b><span class='val'>%s</span></div>"
        "<div class='kv'><b>owner</b><span class='val'>%s</span></div>"
        "<div class='kv'><b>registered</b><span class='val'>h=%d</span></div>"
        "<div class='kv'><b>expires</b><span class='val'>%s</span></div>"
        "</div>",
        safe_name, znam_type_name(e->target_type), safe_val,
        safe_owner, e->reg_height, expires);
    if (n > 0) off += (size_t)n;

    if (ntext > 0 || naddr > 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='card'><h2>Records</h2>");
        if (n > 0) off += (size_t)n;
        for (int i = 0; i < ntext && off < sizeof(body) - 512; i++) {
            char sk[96], sv[280];
            html_escape(sk, sizeof(sk), text[i].key);
            html_escape(sv, sizeof(sv), text[i].value);
            n = snprintf(body + off, sizeof(body) - off,
                "<div class='kv'><b>%s</b><span class='val'>%s</span></div>",
                sk, sv);
            if (n > 0) off += (size_t)n;
        }
        for (int i = 0; i < naddr && off < sizeof(body) - 512; i++) {
            char sv[280];
            html_escape(sv, sizeof(sv), addr[i].address);
            n = snprintf(body + off, sizeof(body) - off,
                "<div class='kv'><b>%s</b><span class='val'>%s</span></div>",
                znam_type_name(addr[i].coin_type), sv);
            if (n > 0) off += (size_t)n;
        }
        n = snprintf(body + off, sizeof(body) - off, "</div>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off,
        "<p><a href='/n/%s'>open site</a> &middot; "
        "<a href='/names'>&larr; all names</a></p></body></html>",
        safe_name);
    if (n > 0) off += (size_t)n;
    return name_html_response(body, off, resp, max);
}

/* ── Register form ──────────────────────────────────────────────── */

size_t name_view_register_form(const char *csrf_tok, int64_t pow_ts,
                               uint8_t *resp, size_t max)
{
    char body[20480];
    size_t off = 0;
    int n = name_body_start(body, sizeof(body), "Register a ZCL Name");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<div class='card'>"
        "<h2>Register a name on-chain</h2>"
        "<p>A name (1-63 chars, lowercase letters, digits and hyphens) is "
        "registered by an OP_RETURN transaction broadcast from this node's "
        "wallet. First-come-first-served. Solving a one-time proof-of-work "
        "puzzle in your browser gates the broadcast against floods.</p>"
        "<form id='nameRegForm' method='post' action='/names/register' "
        "data-pow-ts='%lld' data-pow-bits='%d'>"
        "<input type='hidden' name='csrf_token' value='%s'>"
        "<input type='hidden' name='pow_ts' value='%lld'>"
        "<input type='hidden' name='pow_nonce' id='pow_nonce_field' value=''>"
        "<label>Name:</label>"
        "<input type='text' name='name' placeholder='alice' required>"
        "<label>Target type:</label>"
        "<select name='type'>"
        "<option value='onion'>onion (.onion site)</option>"
        "<option value='taddr'>taddr (t-address)</option>"
        "<option value='zaddr'>zaddr (z-address)</option>"
        "<option value='btc'>btc</option>"
        "<option value='ltc'>ltc</option>"
        "<option value='doge'>doge</option>"
        "<option value='content'>content (file-market hash)</option>"
        "</select>"
        "<label>Target value:</label>"
        "<input type='text' name='value' placeholder='abc123....onion' required>"
        "<br><br>"
        "<button type='submit' class='btn' id='regBtn'>Register</button>"
        "<p id='powStatus' style='color:#888;font-size:12px'></p>"
        "<noscript><p style='color:#f80'>JavaScript is required to solve the "
        "anti-flood proof-of-work puzzle. Scripted clients may solve it "
        "directly: SHA3-256(peer_id || timestamp || nonce) must have %d "
        "leading zero bits, where peer_id=SHA3-256(\"znam:register:pow:\" || "
        "name) and timestamp is the pow_ts value; submit the winning nonce as "
        "pow_nonce.</p></noscript>"
        "</form></div>"
        "<script>%s%s</script></body></html>",
        (long long)pow_ts, FAST_SYNC_POW_BITS,
        csrf_tok, (long long)pow_ts,
        FAST_SYNC_POW_BITS,
        NAME_REG_POW_JS_1, NAME_REG_POW_JS_2);
    if (n > 0) off += (size_t)n;
    return name_html_response(body, off, resp, max);
}

size_t name_view_register_result(const char *name, const char *value,
                                 const char *txid, const char *err,
                                 uint8_t *resp, size_t max)
{
    char body[8192];
    size_t off = 0;
    int n = name_body_start(body, sizeof(body), "Registration");
    if (n > 0) off = (size_t)n;

    char safe_name[128], safe_val[280];
    html_escape(safe_name, sizeof(safe_name), name ? name : "");
    html_escape(safe_val, sizeof(safe_val), value ? value : "");

    if (txid && txid[0]) {
        char safe_txid[80];
        html_escape(safe_txid, sizeof(safe_txid), txid);
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='card'><h2>Broadcast</h2>"
            "<div class='kv'><b>name</b><span class='val'>%s</span></div>"
            "<div class='kv'><b>value</b><span class='val'>%s</span></div>"
            "<div class='kv'><b>txid</b><span class='val'>%s</span></div>"
            "<p>The registration confirms when the transaction is mined and "
            "the ZNAM projection folds it. Then <a href='/names/%s'>its "
            "profile</a> and <code>/n/%s</code> resolve.</p></div>"
            "</body></html>",
            safe_name, safe_val, safe_txid, safe_name, safe_name);
    } else {
        char safe_err[512];
        html_escape(safe_err, sizeof(safe_err), err ? err : "unknown error");
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='card'><h2>Not registered</h2>"
            "<p style='color:#ff8800'>%s</p>"
            "<p><a href='/names/register'>&larr; try again</a></p></div>"
            "</body></html>",
            safe_err);
    }
    if (n > 0) off += (size_t)n;
    return name_html_response(body, off, resp, max);
}

size_t name_view_not_found(const char *name, uint8_t *resp, size_t max)
{
    char body[2048];
    size_t off = 0;
    int n = name_body_start(body, sizeof(body), "Name not found");
    if (n > 0) off = (size_t)n;
    char safe_name[128];
    html_escape(safe_name, sizeof(safe_name), name ? name : "");
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='card'><h2>No such name</h2>"
        "<p><b>%s</b> is not registered.</p>"
        "<p><a href='/names/register'>Register it</a> &middot; "
        "<a href='/names'>&larr; all names</a></p></div></body></html>",
        safe_name);
    if (n > 0) off += (size_t)n;
    return name_error_response("404 Not Found", body, off, resp, max);
}
