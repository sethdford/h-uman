/**
 * Motion 9 scroll-driven entrance animations.
 * Use with .hu-scroll-reveal-stagger on card grids.
 * Call _setupScrollEntrance() in firstUpdated for IntersectionObserver fallback.
 */
import { css } from "lit";

export const scrollEntranceStyles = css`
  /* .hu-scroll-reveal is deliberately absent from the IntersectionObserver fallback
     below: the views' observers only watch .hu-scroll-reveal-stagger > *, so hiding it
     there would leave it invisible. Without view() support it renders statically. */
  @supports (animation-timeline: view()) {
    .hu-scroll-reveal,
    .hu-scroll-reveal-stagger > * {
      animation: hu-card-enter linear both;
      animation-timeline: view();
      animation-range: entry 0% entry 100%;
    }
  }

  @keyframes hu-card-enter {
    from {
      opacity: 0;
      transform: translateY(var(--hu-space-lg));
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  /* IntersectionObserver fallback when animation-timeline not supported */
  @supports not (animation-timeline: view()) {
    .hu-scroll-reveal-stagger > * {
      opacity: 0;
      transform: translateY(var(--hu-space-lg));
      transition:
        opacity var(--hu-duration-normal) var(--hu-ease-spring),
        transform var(--hu-duration-normal) var(--hu-ease-spring);
    }
    .hu-scroll-reveal-stagger > *.entered {
      opacity: 1;
      transform: translateY(0);
    }
  }

  @media (prefers-reduced-motion: reduce) {
    .hu-scroll-reveal,
    .hu-scroll-reveal-stagger > * {
      animation: none !important;
      opacity: 1 !important;
      transform: none !important;
      transition: none !important;
    }
  }
`;

/** Motion 9 stagger: 50ms delay, 300ms cap for list/grid entrance */
export const staggerMotion9Styles = css`
  .hu-stagger-motion9 > * {
    animation: hu-stagger-enter var(--hu-duration-normal) var(--hu-ease-spring) both;
  }
  .hu-stagger-motion9 > *:nth-child(1) {
    animation-delay: 0ms;
  }
  .hu-stagger-motion9 > *:nth-child(2) {
    animation-delay: 50ms;
  }
  .hu-stagger-motion9 > *:nth-child(3) {
    animation-delay: 100ms;
  }
  .hu-stagger-motion9 > *:nth-child(4) {
    animation-delay: 150ms;
  }
  .hu-stagger-motion9 > *:nth-child(5) {
    animation-delay: 200ms;
  }
  .hu-stagger-motion9 > *:nth-child(6) {
    animation-delay: 250ms;
  }
  .hu-stagger-motion9 > *:nth-child(n + 7) {
    animation-delay: 300ms;
  }
  @keyframes hu-stagger-enter {
    from {
      opacity: 0;
      transform: translateY(8px);
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .hu-stagger-motion9 > * {
      animation: none !important;
      opacity: 1 !important;
      transform: none !important;
    }
  }
`;
