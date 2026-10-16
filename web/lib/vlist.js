"use strict";

/* Virtual list: renders only the visible rows (plan §7). Usage:
   html`<${VList} items=${items} height=${42} render=${(item) => ...}/>`
   The container scrolls; rows outside the window are skipped. */

const VList = ({ items, height = 40, render, overscan = 6 }) => {
  const host = preactHooks.useRef(null);
  const [scrollTop, setScrollTop] = preactHooks.useState(0);
  const [viewHeight, setViewHeight] = preactHooks.useState(0);

  const onScroll = () => {
    setScrollTop(host.current ? host.current.scrollTop : 0);
  };
  preactHooks.useEffect(() => {
    if (!host.current) return;
    const resize = () => setViewHeight(host.current.clientHeight);
    resize();
    const observer = new ResizeObserver(resize);
    observer.observe(host.current);
    return () => observer.disconnect();
  }, []);

  const start = Math.max(0, Math.floor(scrollTop / height) - overscan);
  const visibleCount = Math.ceil(viewHeight / height) + overscan * 2;
  const end = Math.min(items.length, start + visibleCount);
  const slice = items.slice(start, end);

  return preact.h(
    "div",
    {
      ref: host,
      onScroll,
      style: { overflow: "auto", height: "100%", position: "relative" },
      role: "list",
    },
    preact.h(
      "div",
      { style: { height: items.length * height + "px", position: "relative" } },
      slice.map((item, index) =>
        preact.h(
          "div",
          {
            key: start + index,
            role: "listitem",
            style: {
              position: "absolute",
              top: (start + index) * height + "px",
              height: height + "px",
              width: "100%",
              boxSizing: "border-box",
            },
          },
          render(item, start + index),
        ),
      ),
    ),
  );
};
