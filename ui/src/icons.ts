import { html } from "lit";

const SVG_ATTRS = `viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"`;

export const icons: Record<string, ReturnType<typeof html>> = {
  grid: html`<svg ${SVG_ATTRS}>
    <rect x="2" y="2" width="6" height="6" />
    <rect x="12" y="2" width="6" height="6" />
    <rect x="2" y="12" width="6" height="6" />
    <rect x="12" y="12" width="6" height="6" />
  </svg>`,
  "message-square": html`<svg ${SVG_ATTRS}>
    <path d="M3 4a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H7l-2 2V4z" />
  </svg>`,
  clock: html`<svg ${SVG_ATTRS}>
    <circle cx="10" cy="10" r="7" />
    <path d="M10 6v4l2 2" />
  </svg>`,
  cpu: html`<svg ${SVG_ATTRS}>
    <rect x="4" y="4" width="12" height="12" rx="1" />
    <path
      d="M7 4V2M13 4V2M7 18v-2M13 18v-2M4 7H2M4 13H2M18 7h2M18 13h2M2 10h2M18 10h2M4 10h12M10 4v12"
    />
  </svg>`,
  box: html`<svg ${SVG_ATTRS}>
    <path d="M3 6l7-3 7 3M3 6v8l7 3 7-3V6M10 3v14" />
  </svg>`,
  mic: html`<svg ${SVG_ATTRS}>
    <path d="M10 1a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" />
    <path d="M15 8v1a5 5 0 0 1-10 0V8" />
    <line x1="10" y1="16" x2="10" y2="19" />
    <line x1="7" y1="19" x2="13" y2="19" />
  </svg>`,
  wrench: html`<svg ${SVG_ATTRS}>
    <path d="M14.7 6.3a1 1 0 0 0 0-1.4l-1.4-1.4a1 1 0 0 0-1.4 0l-5 5a4 4 0 1 0 1.4 1.4l5-5z" />
    <circle cx="6" cy="14" r="2" />
  </svg>`,
  radio: html`<svg ${SVG_ATTRS}>
    <path d="M5 15h10a2 2 0 0 0 2-2V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2z" />
    <path d="M10 12a2 2 0 1 0 0-4 2 2 0 0 0 0 4z" />
    <line x1="2" y1="9" x2="18" y2="9" />
  </svg>`,
  puzzle: html`<svg ${SVG_ATTRS}>
    <path
      d="M4 4h4v4c0 1.1.9 2 2 2h4c1.1 0 2-.9 2-2V4h4v12h-4v-4c0-1.1-.9-2-2-2h-4c-1.1 0-2 .9-2 2v4H4V4z"
    />
  </svg>`,
  timer: html`<svg ${SVG_ATTRS}>
    <circle cx="10" cy="10" r="7" />
    <path d="M10 6v4l3 3" />
  </svg>`,
  settings: html`<svg ${SVG_ATTRS}>
    <circle cx="10" cy="10" r="2.5" />
    <path
      d="M10 3v1M10 16v1M16 10h1M3 10h1M14.5 5.5l.7.7M4.8 15.2l.7.7M15.2 14.5l.7-.7M4.2 4.8l.7-.7M5.5 14.5l-.7.7M15.2 4.8l-.7-.7M4.8 4.2l-.7-.7"
    />
  </svg>`,
  server: html`<svg ${SVG_ATTRS}>
    <rect x="2" y="2" width="16" height="5" rx="1" />
    <rect x="2" y="9" width="16" height="5" rx="1" />
    <line x1="6" y1="5" x2="6" y2="5" />
    <line x1="10" y1="5" x2="10" y2="5" />
    <line x1="6" y1="12" x2="6" y2="12" />
    <line x1="10" y1="12" x2="10" y2="12" />
  </svg>`,
  "bar-chart": html`<svg ${SVG_ATTRS}>
    <path d="M4 17V14M10 17V10M16 17V6" />
  </svg>`,
  terminal: html`<svg ${SVG_ATTRS}>
    <rect x="2" y="3" width="16" height="14" rx="1" />
    <path d="M6 8l4 4-4 4M11 13h3" />
  </svg>`,
  chevron: html`<svg ${SVG_ATTRS}>
    <path d="M12 6l-4 4 4 4" />
  </svg>`,
  "chevron-right": html`<svg ${SVG_ATTRS}>
    <path d="M8 6l4 4-4 4" />
  </svg>`,
  refresh: html`<svg ${SVG_ATTRS}>
    <path d="M3 10a7 7 0 0 1 13.6-2.3M17 10a7 7 0 0 1-13.6 2.3" />
    <path d="M17 3v5h-5M3 17v-5h5" />
  </svg>`,
  search: html`<svg ${SVG_ATTRS}>
    <circle cx="9" cy="9" r="5" />
    <path d="M13 13l4 4" />
  </svg>`,
  zap: html`<svg ${SVG_ATTRS}>
    <path d="M13 2L3 14h7l-1 6 10-12h-7l1-6z" />
  </svg>`,
  "sidebar-toggle": html`<svg ${SVG_ATTRS}>
    <rect x="3" y="3" width="14" height="14" rx="2" />
    <line x1="8" y1="3" x2="8" y2="17" />
  </svg>`,
};
