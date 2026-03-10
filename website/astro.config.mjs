// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import sitemap from "@astrojs/sitemap";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  site: "https://h-uman.ai",
  base: "/",
  prefetch: {
    prefetchAll: true,
    defaultStrategy: "hover",
  },
  integrations: [
    starlight({
      disable404Route: true,
      title: "Human",
      logo: {
        src: "./public/human.svg",
        alt: "Human",
      },
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/sethdford/human",
        },
      ],
      customCss: ["./src/styles/global.css"],
      sidebar: [
        {
          label: "Getting Started",
          items: [
            { label: "Installation", slug: "getting-started/installation" },
            { label: "Quick Start", slug: "getting-started/quickstart" },
            { label: "Configuration", slug: "getting-started/configuration" },
          ],
        },
        {
          label: "Features",
          items: [
            { label: "TUI Mode", slug: "features/tui" },
            { label: "Cron System", slug: "features/cron" },
            { label: "Skills System", slug: "features/skills" },
            { label: "MCP Server Integration", slug: "features/mcp" },
            { label: "Migration", slug: "features/migrate" },
            { label: "Tunnel Providers", slug: "features/tunnels" },
            { label: "Voice Channel", slug: "features/voice" },
          ],
        },
        {
          label: "Deployment",
          items: [{ label: "Docker & Nix", slug: "deployment/docker" }],
        },
        {
          label: "Providers",
          items: [
            { label: "Overview", slug: "providers/overview" },
            { label: "Cloud Providers", slug: "providers/cloud" },
            { label: "Local Models", slug: "providers/local" },
            { label: "OpenAI-Compatible", slug: "providers/compatible" },
          ],
        },
        {
          label: "Channels",
          items: [
            { label: "Overview", slug: "channels/overview" },
            { label: "CLI", slug: "channels/cli" },
            { label: "Telegram", slug: "channels/telegram" },
            { label: "Discord", slug: "channels/discord" },
            { label: "Slack", slug: "channels/slack" },
            { label: "Signal", slug: "channels/signal" },
            { label: "Nostr", slug: "channels/nostr" },
            { label: "Email", slug: "channels/email" },
            { label: "iMessage", slug: "channels/imessage" },
            { label: "IRC", slug: "channels/irc" },
            { label: "Matrix", slug: "channels/matrix" },
            { label: "Web", slug: "channels/web" },
            { label: "More Channels", slug: "channels/more" },
          ],
        },
        {
          label: "Tools",
          slug: "tools/overview",
        },
        {
          label: "Memory",
          items: [
            { label: "Overview", slug: "memory/overview" },
            { label: "FTS5 & Vector Memory", slug: "memory/vector" },
          ],
        },
        {
          label: "Security",
          slug: "security/overview",
        },
        {
          label: "Hardware",
          items: [
            { label: "Overview", slug: "peripherals/overview" },
            { label: "Hardware Peripherals", slug: "peripherals/hardware" },
          ],
        },
        {
          label: "API",
          items: [
            { label: "Overview", slug: "api/overview" },
            { label: "Gateway API", slug: "api/gateway" },
          ],
        },
        {
          label: "Design System",
          items: [
            { label: "Design Tokens", slug: "design-system/tokens" },
            { label: "Color System", slug: "design-system/colors" },
            { label: "Typography", slug: "design-system/typography" },
            { label: "Motion System", slug: "design-system/motion" },
            { label: "Liquid Glass", slug: "design-system/glass" },
            { label: "Components", slug: "design-system/components" },
            { label: "Accessibility", slug: "design-system/accessibility" },
            { label: "Platform Integration", slug: "design-system/platforms" },
          ],
        },
        {
          label: "Contributing",
          slug: "contributing/overview",
        },
      ],
    }),
    sitemap(),
  ],
  vite: {
    plugins: [tailwindcss()],
  },
});
