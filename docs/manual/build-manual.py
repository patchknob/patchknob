#!/usr/bin/env python3
"""Assemble the PatchKnob manual from its section fragments.

Each agent writes one fragment under sections/.  This stitches them together in
filename order, generates the left-hand navigation from the h2/h3 ids they
declare, and writes a single self-contained page -- no build step, no CDN, so
it works from a file:// URL as readily as from the website.
"""
import html
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTIONS = os.path.join(HERE, "sections")
OUT = os.path.join(HERE, "index.html")

def main():
    files = sorted(f for f in os.listdir(SECTIONS) if f.endswith(".html"))
    if not files:
        print("no section fragments found in", SECTIONS, file=sys.stderr)
        return 1

    bodies, nav = [], []
    seen_ids = {}
    for fn in files:
        with open(os.path.join(SECTIONS, fn), encoding="utf-8") as fh:
            frag = fh.read()

        #  Section title: prefer data-title on the <section>, else the first h2.
        m = re.search(r'<section[^>]*\bdata-title="([^"]*)"', frag)
        h2 = re.search(r'<h2[^>]*\bid="([^"]+)"[^>]*>(.*?)</h2>', frag, re.S)
        if not h2:
            print("warning: %s has no <h2 id=...>, skipped" % fn, file=sys.stderr)
            continue
        sec_id = h2.group(1)
        title = m.group(1) if m else re.sub(r"<[^>]+>", "", h2.group(2)).strip()

        subs = []
        for sm in re.finditer(r'<h3[^>]*\bid="([^"]+)"[^>]*>(.*?)</h3>', frag, re.S):
            subs.append((sm.group(1), re.sub(r"<[^>]+>", "", sm.group(2)).strip()))

        #  Duplicate ids would silently break navigation, so say so loudly.
        for i, _ in [(sec_id, title)] + subs:
            if i in seen_ids:
                print("warning: duplicate id '%s' in %s (also in %s)"
                      % (i, fn, seen_ids[i]), file=sys.stderr)
            seen_ids[i] = fn

        nav.append('    <li><a href="#%s">%s</a>' % (sec_id, html.escape(title)))
        if subs:
            nav.append("      <ul>")
            nav += ['        <li><a href="#%s">%s</a></li>' % (i, html.escape(t))
                    for i, t in subs]
            nav.append("      </ul>")
        nav.append("    </li>")
        #  The FORMAT example put the same id on <section> and its <h2>, which
        #  is duplicate-id invalid HTML and makes the anchor ambiguous.  Strip
        #  it from the wrapper and let the heading own it -- done here rather
        #  than asking five agents to each remember, because the builder can
        #  simply guarantee it.
        frag = re.sub(r'(<section\b[^>]*?)\s+id="%s"' % re.escape(sec_id),
                      r'\1', frag, count=1)
        bodies.append(frag.strip())

    page = SHELL.replace("{{NAV}}", "\n".join(nav)) \
                .replace("{{BODY}}", "\n\n".join(bodies))
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(page)
    print("wrote %s  (%d sections, %d headings)" % (OUT, len(bodies), len(seen_ids)))
    return 0

SHELL = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PatchKnob Manual</title>
<style>
  :root {
    --bg:#0d1014; --panel:#141920; --edge:rgba(255,255,255,.09);
    --text:#dfe6ee; --muted:#8c99a8; --accent:#39ff88; --accentdim:#1f7d47;
    --code:#0a0d11;
  }
  * { box-sizing:border-box; }
  html { scroll-behavior:smooth; }
  body {
    margin:0; background:var(--bg); color:var(--text);
    font:16px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Ubuntu,sans-serif;
  }
  a { color:var(--accent); text-decoration:none; }
  a:hover { text-decoration:underline; }

  .layout { display:grid; grid-template-columns:296px minmax(0,1fr); }

  /* ---- left-hand navigation ---- */
  .sidebar {
    position:sticky; top:0; height:100vh; overflow-y:auto;
    background:var(--panel); border-right:1px solid var(--edge); padding:22px 0 60px;
  }
  .brand {
    display:flex; align-items:center; gap:10px;
    padding:0 20px 16px; font-weight:700; letter-spacing:.2px;
  }
  .brand .mark {
    display:grid; place-items:center; width:28px; height:28px; border-radius:7px;
    background:var(--accentdim); color:#04120a; font-size:.8rem; font-weight:800;
  }
  .search { padding:0 20px 14px; }
  .search input {
    width:100%; padding:8px 10px; border-radius:7px; color:var(--text);
    background:var(--code); border:1px solid var(--edge); font-size:.92rem;
  }
  .search input::placeholder { color:var(--muted); }
  .sidebar ul { list-style:none; margin:0; padding:0; }
  .sidebar > ul { padding:0 10px; }
  .sidebar > ul > li { margin:2px 0 10px; }
  .sidebar > ul > li > a {
    display:block; padding:7px 10px; border-radius:7px;
    color:var(--text); font-weight:650; font-size:.95rem;
  }
  .sidebar ul ul { margin:2px 0 0 10px; border-left:1px solid var(--edge); }
  .sidebar ul ul a {
    display:block; padding:5px 12px; color:var(--muted); font-size:.9rem;
  }
  .sidebar a:hover { background:rgba(255,255,255,.05); text-decoration:none; }
  .sidebar a.active { color:var(--accent); background:rgba(57,255,136,.08); }
  .sidebar li.hidden { display:none; }

  /* ---- content ---- */
  main { padding:44px 46px 140px; max-width:900px; }
  h1 { font-size:2rem; margin:0 0 6px; }
  .sub { color:var(--muted); margin:0 0 34px; }
  section { padding-top:26px; border-top:1px solid var(--edge); margin-top:34px; }
  section:first-of-type { border-top:0; margin-top:0; }
  h2 { font-size:1.5rem; margin:0 0 12px; scroll-margin-top:20px; }
  h3 { font-size:1.13rem; margin:30px 0 8px; scroll-margin-top:20px; }
  h4 { font-size:1rem; margin:20px 0 6px; color:var(--muted); }
  p, li { color:var(--text); }
  code {
    background:var(--code); border:1px solid var(--edge); border-radius:4px;
    padding:1px 5px; font-size:.9em;
  }
  pre {
    background:var(--code); border:1px solid var(--edge); border-radius:8px;
    padding:13px 15px; overflow-x:auto;
  }
  pre code { border:0; padding:0; background:none; }
  kbd {
    background:#1c232c; border:1px solid var(--edge); border-bottom-width:2px;
    border-radius:5px; padding:1px 6px; font-size:.85em; font-family:inherit;
    white-space:nowrap;
  }
  table { border-collapse:collapse; width:100%; margin:14px 0; font-size:.95rem; }
  th, td { border:1px solid var(--edge); padding:8px 11px; text-align:left; vertical-align:top; }
  th { background:rgba(255,255,255,.04); }
  blockquote {
    margin:16px 0; padding:11px 16px; border-left:3px solid var(--accentdim);
    background:rgba(57,255,136,.05); color:var(--text);
  }
  blockquote p:first-child { margin-top:0; }
  blockquote p:last-child { margin-bottom:0; }
  .toplink { display:block; margin-top:60px; color:var(--muted); font-size:.9rem; }

  @media (max-width:900px) {
    .layout { grid-template-columns:1fr; }
    .sidebar { position:static; height:auto; border-right:0; border-bottom:1px solid var(--edge); }
    main { padding:28px 20px 90px; }
  }
</style>
</head>
<body>
<div class="layout">
  <aside class="sidebar">
    <div class="brand"><span class="mark">PK</span> PatchKnob Manual</div>
    <div class="search"><input id="filter" type="search" placeholder="Filter sections&hellip;" aria-label="Filter sections"></div>
    <nav>
      <ul>
{{NAV}}
      </ul>
    </nav>
  </aside>

  <main>
    <h1>PatchKnob Manual</h1>
    <p class="sub">Version 0.8.7 &mdash; every feature, and where it falls short.</p>

{{BODY}}

    <a class="toplink" href="#">Back to top</a>
  </main>
</div>

<script>
//  Highlight the section you are reading.  IntersectionObserver rather than a
//  scroll handler so it costs nothing while you are just reading.
(function () {
  var links = {};
  document.querySelectorAll('.sidebar a[href^="#"]').forEach(function (a) {
    links[a.getAttribute('href').slice(1)] = a;
  });
  var targets = Object.keys(links)
    .map(function (id) { return document.getElementById(id); })
    .filter(Boolean);
  var current = null;
  var io = new IntersectionObserver(function (entries) {
    entries.forEach(function (e) {
      if (!e.isIntersecting) return;
      var a = links[e.target.id];
      if (!a || a === current) return;
      if (current) current.classList.remove('active');
      a.classList.add('active');
      current = a;
    });
  }, { rootMargin: '0px 0px -75% 0px', threshold: 0 });
  targets.forEach(function (t) { io.observe(t); });

  //  Filter box: hides navigation entries that do not match, keeping a parent
  //  visible when one of its children does.
  var filter = document.getElementById('filter');
  filter.addEventListener('input', function () {
    var q = filter.value.trim().toLowerCase();
    document.querySelectorAll('.sidebar > nav > ul > li').forEach(function (top) {
      var kids = top.querySelectorAll('ul li');
      var anyKid = false;
      kids.forEach(function (li) {
        var hit = !q || li.textContent.toLowerCase().indexOf(q) !== -1;
        li.classList.toggle('hidden', !hit);
        if (hit) anyKid = true;
      });
      var selfHit = !q || top.querySelector('a').textContent.toLowerCase().indexOf(q) !== -1;
      top.classList.toggle('hidden', !(selfHit || anyKid));
      if (selfHit && q) kids.forEach(function (li) { li.classList.remove('hidden'); });
    });
  });
})();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
