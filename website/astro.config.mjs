// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  site: "https://sethdford.github.io",
  base: "/seaclaw",
  integrations: [
    starlight({
      title: "SeaClaw",
      logo: {
        src: "./public/seaclaw.svg",
        alt: "SeaClaw",
      },
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/sethdford/seaclaw",
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
          slug: "memory/overview",
        },
        {
          label: "Security",
          slug: "security/overview",
        },
        {
          label: "Hardware",
          slug: "peripherals/overview",
        },
        {
          label: "Gateway API",
          slug: "api/overview",
        },
        {
          label: "Contributing",
          slug: "contributing/overview",
        },
      ],
    }),
  ],
  vite: {
    plugins: [tailwindcss()],
  },
});
