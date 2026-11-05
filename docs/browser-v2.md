# browser v2 - design (researched, 2026-11-05)

## what the field converged on
researched browser-use, stagehand, dev-browser, cdp/bidi. every serious agent-facing
browser layer does the same three things:
1. expose the page through the ACCESSIBILITY TREE (cdp Accessibility.getFullAXTree),
   not raw css selectors or tag soup. role + accessible name is semantic, survives
   markup/css-class churn, and matches how a sighted human reads the page.
2. assign STABLE indices to INTERACTIVE + VISIBLE elements only (buttons, links,
   inputs, selects, tabs, menu items, slider, contenteditable, cursor:pointer).
   non-interactive content is text context, not numbered.
3. render each interactive node as ONE compact line: [n] role "name" + state
   (disabled / checked / expanded / value). the model acts by index; coordinates
   are the fallback for canvas/svg.

## why my v1 (browser_native.cpp) is below that bar
observe = querySelectorAll('a,button,input,...') + getBoundingClientRect + computed
style. it is tag-driven, so it misses a <div role=link>, misweights decorative nodes,
and gives the model raw tags/ids instead of semantics. it works, but it is the old
approach. the ax-tree approach is strictly more robust and more compact.

## what stays (proven, not the 'method' under research)
chrome discovery + launch (headless / headed-under-xvfb), free-port pick, /json/list
wait, cdp websocket request/response with id-matching + event pump, stealth init
script, screenshot, idle shutdown. this is commodity transport, not the thing being
improved - rewritten clean, not re-invented. (a full re-implementation of the
websocket client would be pure churn with regression risk and no benefit to the
agent-facing method.)

## what is rebuilt from scratch (the actual 'best way')
- observe(): ax.enable + ax.getFullAXTree -> walk nodes -> {role,name,value,
  description,ignored,backendDOMNodeId}. resolve geometry for interactive nodes.
  index the interactive+visible set. render [n] role "name" [state]. keep page text.
- interactivity: role in the interactive-role set, or tag in {a,button,input,select,
  textarea,summary}, or aria role, or cursor:pointer.
- find(text): filter the indexed set by name/aria/placeholder/value without re-scanning.
- resolve(n): index -> {x,y,backendDOMNodeId} for actions (kept in a selector map
  for the life of the page, re-built on navigate/observe).
- actions: navigate, go_back, go_forward, reload, click, click_xy, type (clear+insert),
  type_text, press, select(n,value), hover, drag, drag_xy, scroll, read, eval,
  screenshot, status, restart, close.

## validation (production path, per the self-mod rules)
1. ax probe: dump getFullAXTree on a local fixture, confirm field names
   (role.value, name.value, childIds, backendDOMNodeId, ignored) before parsing.
2. fixture: navigate -> observe (ax) -> find -> click -> type -> read, all through
   the registered tool (find_tool + run), not a re-implementation.
3. real page: one live site, same path.
4. cut over: register as 'browser', remove browser_native.cpp, rebuild, re-run the
   full suite on the new registration.
