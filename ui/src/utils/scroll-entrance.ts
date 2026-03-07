const OBSERVED = new WeakSet<Element>();

const observer =
  typeof IntersectionObserver !== "undefined"
    ? new IntersectionObserver(
        (entries) => {
          for (const entry of entries) {
            if (entry.isIntersecting) {
              entry.target.classList.add("sc-visible");
              observer!.unobserve(entry.target);
            }
          }
        },
        { threshold: 0.1, rootMargin: "0px 0px -40px 0px" },
      )
    : null;

export function observeEntrance(el: Element): void {
  if (!observer || OBSERVED.has(el)) return;
  OBSERVED.add(el);
  el.classList.add("sc-entrance");
  observer.observe(el);
}

export function observeAllCards(root: ShadowRoot | Document): void {
  const cards = root.querySelectorAll("sc-card, .sc-observe");
  cards.forEach(observeEntrance);
}

export function unobserveAllCards(root: ShadowRoot | Document): void {
  if (!observer) return;
  const cards = root.querySelectorAll("sc-card, .sc-observe");
  cards.forEach((el) => observer!.unobserve(el));
}
