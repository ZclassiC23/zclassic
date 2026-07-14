/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: render escaped, proof-oriented HTML for the public Blog resource. */

#include "views/blog_post_view.h"

#include "views/format_helpers.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct blog_view_sink {
    char *out;
    size_t capacity;
    size_t used;
    bool ok;
};

static void sink_bytes(struct blog_view_sink *sink,
                       const char *bytes, size_t len)
{
    if (!sink->ok || len > sink->capacity - sink->used - 1) {
        sink->ok = false;
        return;
    }
    memcpy(sink->out + sink->used, bytes, len);
    sink->used += len;
    sink->out[sink->used] = 0;
}

static void sink_literal(struct blog_view_sink *sink, const char *text)
{
    sink_bytes(sink, text, strlen(text));
}

static void sink_format(struct blog_view_sink *sink, const char *fmt, ...)
{
    if (!sink->ok)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sink->out + sink->used,
                      sink->capacity - sink->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sink->capacity - sink->used) {
        sink->ok = false;
        return;
    }
    sink->used += (size_t)n;
}

static void sink_html(struct blog_view_sink *sink, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text;
         sink->ok && *p; p++) {
        switch (*p) {
        case '&': sink_literal(sink, "&amp;"); break;
        case '<': sink_literal(sink, "&lt;"); break;
        case '>': sink_literal(sink, "&gt;"); break;
        case '"': sink_literal(sink, "&quot;"); break;
        case '\'': sink_literal(sink, "&#39;"); break;
        default: sink_bytes(sink, (const char *)p, 1); break;
        }
    }
}

static void hex32(const uint8_t bytes[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 15];
    }
    out[64] = 0;
}

static const char *view_status_name(enum blog_publication_status status)
{
    switch (status) {
    case BLOG_PUBLICATION_CONFIRMED: return "projection-confirmed";
    case BLOG_PUBLICATION_ORPHANED: return "orphaned";
    case BLOG_PUBLICATION_UNRESOLVED:
    default: return "unresolved";
    }
}

static void blog_document_open(struct blog_view_sink *sink,
                               const char *title)
{
    sink_literal(sink,
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='color-scheme' content='dark'>"
        "<style>"
        ":root{--ink:#f8fafc;--muted:#94a3b8;--line:rgba(148,163,184,.18);"
        "--panel:rgba(15,23,42,.72);--panel2:rgba(30,41,59,.52);"
        "--blue:#38bdf8;--violet:#a78bfa;--green:#4ade80;--amber:#fbbf24}"
        "*{box-sizing:border-box}html{background:#05070b;scroll-behavior:smooth}"
        "body{margin:0;min-height:100vh;color:var(--ink);font:16px/1.65 "
        "ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
        "background:radial-gradient(circle at 14% -8%,rgba(56,189,248,.18),transparent 34rem),"
        "radial-gradient(circle at 92% 8%,rgba(167,139,250,.16),transparent 30rem),#05070b}"
        "body:before{content:'';position:fixed;inset:0;pointer-events:none;opacity:.22;"
        "background-image:linear-gradient(rgba(255,255,255,.025) 1px,transparent 1px),"
        "linear-gradient(90deg,rgba(255,255,255,.025) 1px,transparent 1px);"
        "background-size:44px 44px;mask-image:linear-gradient(to bottom,#000,transparent 72%)}"
        "a{color:inherit;text-decoration:none}.shell{position:relative;width:min(1080px,calc(100% - 32px));"
        "margin:auto}.skip{position:fixed;left:16px;top:-80px;z-index:20;padding:10px 14px;"
        "border-radius:10px;background:#fff;color:#020617}.skip:focus{top:16px}"
        ".nav{display:flex;align-items:center;justify-content:space-between;padding:24px 0;"
        "border-bottom:1px solid var(--line)}.brand{display:flex;align-items:center;gap:12px;"
        "font-weight:750;letter-spacing:-.02em}.mark{display:grid;place-items:center;width:36px;height:36px;"
        "border:1px solid rgba(125,211,252,.45);border-radius:11px;color:#bae6fd;"
        "background:linear-gradient(145deg,rgba(56,189,248,.2),rgba(167,139,250,.16));"
        "box-shadow:0 0 28px rgba(56,189,248,.12)}.network{display:flex;align-items:center;gap:8px;"
        "color:var(--muted);font-size:.78rem;text-transform:uppercase;letter-spacing:.12em}"
        ".dot{width:7px;height:7px;border-radius:50%;background:var(--green);"
        "box-shadow:0 0 13px rgba(74,222,128,.8)}.hero{display:grid;grid-template-columns:minmax(0,1fr) 260px;"
        "gap:48px;align-items:end;padding:76px 0 58px}.eyebrow{margin:0 0 14px;color:#7dd3fc;"
        "font-size:.75rem;font-weight:750;text-transform:uppercase;letter-spacing:.16em}"
        "h1,h2,p{margin-top:0}h1{max-width:780px;margin-bottom:20px;font-size:clamp(2.55rem,7vw,5.4rem);"
        "line-height:.98;letter-spacing:-.055em}.gradient{background:linear-gradient(92deg,#f8fafc 10%,#7dd3fc 54%,#c4b5fd);"
        "-webkit-background-clip:text;background-clip:text;color:transparent}.lede{max-width:680px;"
        "margin-bottom:24px;color:#cbd5e1;font-size:clamp(1rem,2vw,1.18rem)}"
        ".chips{display:flex;flex-wrap:wrap;gap:9px}.chip,.badge{display:inline-flex;align-items:center;"
        "gap:7px;border:1px solid var(--line);border-radius:999px;background:rgba(15,23,42,.62);"
        "padding:6px 10px;color:#cbd5e1;font-size:.75rem}.stat{border:1px solid var(--line);"
        "border-radius:22px;padding:24px;background:linear-gradient(145deg,rgba(30,41,59,.72),rgba(15,23,42,.42));"
        "box-shadow:0 24px 80px rgba(0,0,0,.24)}.stat strong{display:block;font-size:2.5rem;"
        "line-height:1;letter-spacing:-.05em}.stat span{display:block;margin-top:9px;color:var(--muted);font-size:.82rem}");
    sink_literal(sink,
        ".section-head{display:flex;justify-content:space-between;gap:20px;align-items:end;margin-bottom:18px}"
        ".section-head h2{margin:0;font-size:1.45rem;letter-spacing:-.025em}.muted,.meta{color:var(--muted)}"
        ".meta{font-size:.8rem}.feed{display:grid;gap:12px}.post-card{display:grid;grid-template-columns:1fr auto;"
        "gap:20px;align-items:center;padding:24px;border:1px solid var(--line);border-radius:18px;"
        "background:var(--panel);backdrop-filter:blur(14px);transition:border-color .18s,transform .18s,background .18s}"
        ".post-card:hover{transform:translateY(-2px);border-color:rgba(125,211,252,.42);background:rgba(30,41,59,.78)}"
        ".post-card h3{margin:6px 0 7px;font-size:1.25rem;letter-spacing:-.025em}.identity{color:#7dd3fc;"
        "font-size:.78rem;font-weight:700}.arrow{display:grid;place-items:center;width:38px;height:38px;"
        "border:1px solid var(--line);border-radius:50%;color:#7dd3fc}.empty{padding:42px;border:1px dashed rgba(148,163,184,.3);"
        "border-radius:18px;text-align:center;background:rgba(15,23,42,.42)}"
        ".article-wrap{width:min(820px,100%);margin:58px auto}.article{border:1px solid var(--line);"
        "border-radius:24px;background:var(--panel);padding:clamp(24px,5vw,58px);box-shadow:0 34px 100px rgba(0,0,0,.28)}"
        ".article h1{font-size:clamp(2.2rem,7vw,4.6rem)}.article-meta{display:flex;flex-wrap:wrap;gap:8px 18px;"
        "padding-bottom:28px;border-bottom:1px solid var(--line)}.article-body{padding:34px 0 12px;"
        "font-size:1.08rem}.article-body pre{margin:0;white-space:pre-wrap;overflow-wrap:anywhere;font:inherit;color:#e2e8f0}"
        ".proof{margin-top:24px;padding:24px;border:1px solid rgba(125,211,252,.22);border-radius:20px;"
        "background:linear-gradient(145deg,rgba(14,116,144,.11),rgba(76,29,149,.1))}"
        ".proof-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin-top:18px}"
        ".proof-item{min-width:0;padding:15px;border:1px solid var(--line);border-radius:14px;background:rgba(2,6,23,.38)}"
        ".proof-label{display:flex;align-items:center;gap:8px;margin-bottom:5px;font-size:.72rem;font-weight:750;"
        "text-transform:uppercase;letter-spacing:.1em}.proof-item p{margin:0;color:#cbd5e1;font-size:.82rem}"
        ".good{color:var(--green)}.pending{color:var(--amber)}.status-dot{width:7px;height:7px;border-radius:50%;"
        "background:currentColor;box-shadow:0 0 10px currentColor}.hash,code{overflow-wrap:anywhere;color:#bae6fd;"
        "font:500 .76rem/1.55 ui-monospace,SFMono-Regular,Menlo,monospace}footer{padding:42px 0 54px;"
        "color:var(--muted);font-size:.8rem}a:focus-visible{outline:3px solid #7dd3fc;outline-offset:4px;border-radius:6px}"
        "@media(max-width:720px){.network{display:none}.hero{grid-template-columns:1fr;gap:24px;padding:52px 0 38px}"
        ".stat{display:flex;align-items:center;gap:14px}.stat span{margin:0}.proof-grid{grid-template-columns:1fr}"
        ".post-card{padding:19px}.article-wrap{margin:32px auto}.article{border-radius:18px}}"
        "@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;transition:none!important}}"
        "</style><title>");
    sink_html(sink, title);
    sink_literal(sink, "</title></head><body><a class='skip' href='#content'>Skip to content</a>");
}

static void blog_nav(struct blog_view_sink *sink, const char *back_name)
{
    sink_literal(sink, "<div class='shell'><nav class='nav' aria-label='Primary'><a class='brand' href='/blog'>"
                       "<span class='mark' aria-hidden='true'>Z</span><span>ZClassic23 Journal</span></a>");
    if (back_name && back_name[0]) {
        sink_literal(sink, "<a class='network' href='/blog/");
        sink_html(sink, back_name);
        sink_literal(sink, "' aria-label='Back to author blog'>← @");
        sink_html(sink, back_name);
        sink_literal(sink, "</a>");
    } else {
        sink_literal(sink, "<span class='network'><span class='dot'></span>local sovereign view</span>");
    }
    sink_literal(sink, "</nav>");
}

size_t blog_post_index_view_render(const struct blog_post_index_page *page,
                                   uint8_t *out, size_t out_capacity)
{
    if (!page || !out || out_capacity < 2)
        return 0;
    struct blog_view_sink sink = {
        .out = (char *)out,
        .capacity = out_capacity,
        .used = 0,
        .ok = true,
    };
    sink.out[0] = 0;
    blog_document_open(&sink, page->blog_name[0]
        ? "ZNAM Blog — ZClassic23" : "ZClassic23 Journal");
    blog_nav(&sink, NULL);
    sink_literal(&sink, "<header class='hero'><div><p class='eyebrow'>Wallet-signed publishing demo</p><h1><span class='gradient'>");
    if (page->blog_name[0]) {
        sink_literal(&sink, "Writing by @");
        sink_html(&sink, page->blog_name);
    } else {
        sink_literal(&sink, "Ideas with cryptographic provenance.");
    }
    sink_literal(&sink, "</span></h1><p class='lede'>Portable, verifiable writing rendered directly by a C23 full node. "
                        "Wallet keys establish authorship; ZNAM provides the human identity.</p>"
                        "<div class='chips'><span class='chip'>C23 native</span><span class='chip'>secp256k1 signed</span>"
                        "<span class='chip'>ZNAM named</span><span class='chip'>onion available</span></div></div>"
                        "<aside class='stat' aria-label='Local post count'><strong>");
    sink_format(&sink, "%d", page->count);
    sink_literal(&sink, "</strong><span>canonical post routes in this local view</span></aside></header>"
                        "<main id='content'><div class='section-head'><div><p class='eyebrow'>Local collection</p>"
                        "<h2>Latest accepted events</h2></div><span class='meta'>/blog · clearnet + onion</span></div>");
    if (page->count == 0) {
        sink_literal(&sink, "<div class='empty'><h2>No posts on this node yet</h2>"
                            "<p class='muted'>A verified signed event will appear here after local import.</p></div>");
    } else {
        sink_literal(&sink, "<section class='feed' aria-label='Blog posts'>");
    }
    for (int i = 0; i < page->count && sink.ok; i++) {
        const struct db_blog_post_summary *post = &page->posts[i];
        char when[40];
        zcl_format_time(when, sizeof(when), post->event_created_at);
        sink_literal(&sink, "<a class='post-card' href='/blog/");
        sink_html(&sink, post->blog_name);
        sink_literal(&sink, "/");
        sink_html(&sink, post->slug);
        sink_literal(&sink, "'><article><span class='identity'>@");
        sink_html(&sink, post->blog_name);
        sink_literal(&sink, "</span><h3>");
        sink_html(&sink, post->title);
        sink_format(&sink, "</h3><p class='meta'>sequence %llu · ",
                    (unsigned long long)post->sequence);
        sink_html(&sink, when[0] ? when : "time unavailable");
        sink_literal(&sink, "</p></article><span class='arrow' aria-hidden='true'>→</span></a>");
    }
    if (page->count > 0)
        sink_literal(&sink, "</section>");
    sink_literal(&sink,
        "</main><footer>Content is locally stored and independently verifiable; network anti-entropy remains a separate availability proof. "
        "Mounted at <code>/blog</code> on zclnet.net and participating node onions.</footer></div></body></html>");
    if (!sink.ok) {
        out[0] = 0;
        return 0;
    }
    return sink.used;
}

size_t blog_post_view_render(const struct blog_post_page *page,
                             uint8_t *out, size_t out_capacity)
{
    if (!page || !out || out_capacity < 2)
        return 0;
    struct blog_view_sink sink = {
        .out = (char *)out,
        .capacity = out_capacity,
        .used = 0,
        .ok = true,
    };
    sink.out[0] = 0;
    char event_hex[65], when[40];
    hex32(page->post.event_id, event_hex);
    zcl_format_time(when, sizeof(when), page->post.event_created_at);
    blog_document_open(&sink, page->post.title);
    blog_nav(&sink, page->post.blog_name);
    sink_literal(&sink, "<main id='content' class='article-wrap'><article class='article'><header>"
                        "<p class='eyebrow'>Signed entry · @");
    sink_html(&sink, page->post.blog_name);
    sink_literal(&sink, "</p><h1><span class='gradient'>");
    sink_html(&sink, page->post.title);
    sink_literal(&sink, "</span></h1><div class='article-meta'><span class='badge'>sequence ");
    sink_format(&sink, "%llu", (unsigned long long)page->post.sequence);
    sink_literal(&sink, "</span><time class='badge'>");
    sink_html(&sink, when[0] ? when : "time unavailable");
    sink_literal(&sink, "</time><span class='badge'>author <code>");
    sink_html(&sink, page->post.author_address);
    sink_literal(&sink, "</code></span></div></header><div class='article-body'><pre>");
    sink_html(&sink, page->post.body);
    sink_literal(&sink, "</pre></div><section class='proof' aria-labelledby='proof-title'>"
                        "<p class='eyebrow'>Verification receipt</p><h2 id='proof-title'>What this node can prove</h2>"
                        "<div class='proof-grid'><div class='proof-item'><div class='proof-label good'>"
                        "<span class='status-dot'></span>Signature verified</div>"
                        "<p>The canonical event ID and secp256k1 signature were recomputed for this read.</p></div>"
                        "<div class='proof-item'><div class='proof-label pending'><span class='status-dot'></span>"
                        "ZNAM admission observed</div><p>The signer matched this node’s mutable ZNAM projection at import. "
                        "Historical owner-epoch chain proof is still pending.</p></div>");
    if (!page->has_receipt) {
        sink_literal(&sink,
            "<div class='proof-item'><div class='proof-label pending'><span class='status-dot'></span>"
            "Chain anchor unresolved</div><p>No fresh exact-script projection match is currently available.</p></div>");
    } else {
        char txid_hex[65], znam_txid_hex[65], observed_when[40];
        hex32(page->receipt.txid, txid_hex);
        hex32(page->receipt.znam_reg_txid, znam_txid_hex);
        zcl_format_time(observed_when, sizeof(observed_when),
                        page->receipt.observed_at);
        sink_format(&sink, "<div class='proof-item'><div class='proof-label %s'>"
                    "<span class='status-dot'></span>Chain %s</div><p>Height %lld · observed ",
                    page->receipt.status == BLOG_PUBLICATION_CONFIRMED
                        ? "good" : "pending",
                    view_status_name(page->receipt.status),
                    (long long)page->receipt.block_height);
        sink_html(&sink, observed_when[0] ? observed_when : "time unavailable");
        sink_format(&sink, "<br><code class='hash'>%s</code><br>ZNAM observation <code class='hash'>%s</code></p></div>",
                    txid_hex, znam_txid_hex);
    }
    sink_format(&sink,
        "<div class='proof-item'><div class='proof-label %s'><span class='status-dot'></span>"
        "Served frontier</div><p>served-frontier proof: %s. H* + active-slot/body verification is required.</p></div>"
        "<div class='proof-item'><div class='proof-label %s'><span class='status-dot'></span>"
        "Content %s</div><p>The signed article body is %s on this node; anchoring alone does not provide it.</p></div>"
        "</div><p class='meta' style='margin:18px 0 0'>Event <code class='hash'>%s</code></p>"
        "</section></article></main><footer>Projection evidence is deliberately separated from consensus finality.</footer>"
        "</div></body></html>",
        page->served_frontier_proven ? "good" : "pending",
        page->served_frontier_proven ? "proven" : "pending",
        page->content_available ? "good" : "pending",
        page->content_available ? "available" : "unavailable",
        page->content_available ? "available" : "unavailable",
        event_hex);
    if (!sink.ok) {
        out[0] = 0;
        return 0;
    }
    return sink.used;
}
