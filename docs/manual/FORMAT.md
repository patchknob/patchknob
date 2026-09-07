# Manual section format — read this before writing

Five agents write five sections of one manual. It has to read as a single
document, so the format is fixed.

## Your output

ONE file: `docs/manual/sections/<NN>-<slug>.html`, an HTML **fragment** — no
`<!doctype>`, no `<html>`, `<head>` or `<body>`. The shell supplies those.

Structure:

```html
<section data-title="Arrangement &amp; Editing">
  <h2 id="arrange">Arrangement &amp; Editing</h2>
  <p>One or two sentences on what this part of PatchKnob is for.</p>

  <h3 id="arrange-clips">Working with clips</h3>
  <p>...</p>

  <h3 id="arrange-fades">Fades and crossfades</h3>
  <p>...</p>
</section>
```

* The `<h2>` owns the section anchor; do not repeat its id on the
  `<section>` wrapper (two elements sharing an id is invalid HTML). The builder
  strips it if you do.
* Every `<h2>` and `<h3>` needs a **stable, unique id**, prefixed with your
  section slug (`arrange-fades`, not `fades`). The left-hand navigation is
  generated from these, so an id collision breaks another agent's links.
* `<h3>` is the deepest level that appears in the navigation. Use `<h4>` freely
  below it.
* Allowed elements: `p`, `ul`/`ol`/`li`, `table`/`thead`/`tbody`/`tr`/`th`/`td`,
  `code`, `pre`, `strong`, `em`, `kbd`, `a`, `blockquote`.
* Keyboard shortcuts go in `<kbd>` — `<kbd>Ctrl</kbd>+<kbd>Z</kbd>`.
* No inline `style=`, no `<script>`, no images. The shell styles everything.

## What to write

**Document what the code actually does.** Read the source. Every shortcut,
menu item and parameter you describe must exist — open the key handler and
check. A manual that documents an imagined feature is worse than no manual,
because it sends people looking for something that was never there.

* Lead each topic with what the user is trying to *do*, then how.
* Give exact key combinations, exact menu paths, exact parameter ranges.
* Where behaviour is surprising, say why. (Example: an automation lane inherits
  the track of the lane above it — worth stating, because nothing on screen
  says so.)
* **Be honest about gaps.** If something is unimplemented, partly working or a
  known trap, say so plainly in a `<blockquote>`. Do not paper over it.
* No marketing. No "powerful", no "seamlessly", no exclamation marks. Plain
  description that respects the reader's time.
* British or American spelling — just be internally consistent.

## Scope discipline

Stay inside your assigned area. If you need to mention something another
section owns, link to it by id: `<a href="#mixer-routing">routing</a>`. Do not
document it yourself; you will contradict the agent who owns it.
