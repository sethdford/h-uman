/// <reference types="vite/client" />

declare module "https://esm.sh/chart.js@4" {
  const Chart: new (el: HTMLCanvasElement, config: unknown) => { destroy(): void; resize(): void };
  export default Chart;
}
