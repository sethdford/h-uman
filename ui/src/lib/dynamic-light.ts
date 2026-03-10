const LERP_FACTOR = 0.08;
const DEFAULT_X = 30;
const DEFAULT_Y = 20;
const REDUCED_MOTION_X = 50;
const REDUCED_MOTION_Y = 30;

function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

function prefersReducedMotion(): boolean {
  return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

export class DynamicLight {
  private targetX = DEFAULT_X;
  private targetY = DEFAULT_Y;
  private currentX = DEFAULT_X;
  private currentY = DEFAULT_Y;
  private intensity = 1;
  private rafId: number | null = null;
  private reducedMotion = false;
  private started = false;

  private boundMouseMove = this.handleMouseMove.bind(this);
  private boundDeviceOrientation = this.handleDeviceOrientation.bind(this);
  private boundTick = this.tick.bind(this);

  start(): void {
    this.started = true;
    this.reducedMotion = prefersReducedMotion();
    if (this.reducedMotion) {
      this.setStaticPosition(REDUCED_MOTION_X, REDUCED_MOTION_Y);
      return;
    }

    this.targetX = DEFAULT_X;
    this.targetY = DEFAULT_Y;
    this.currentX = DEFAULT_X;
    this.currentY = DEFAULT_Y;
    this.applyProperties();

    window.addEventListener("mousemove", this.boundMouseMove);
    if (typeof DeviceOrientationEvent !== "undefined") {
      window.addEventListener("deviceorientation", this.boundDeviceOrientation);
    }

    this.rafId = requestAnimationFrame(this.boundTick);
  }

  stop(): void {
    this.started = false;
    window.removeEventListener("mousemove", this.boundMouseMove);
    window.removeEventListener("deviceorientation", this.boundDeviceOrientation);

    if (this.rafId !== null) {
      cancelAnimationFrame(this.rafId);
      this.rafId = null;
    }

    const root = document.documentElement;
    root.style.removeProperty("--hu-light-x");
    root.style.removeProperty("--hu-light-y");
    root.style.removeProperty("--hu-light-intensity");
  }

  setIntensity(value: number): void {
    this.intensity = value;
    if (this.reducedMotion || this.rafId !== null) {
      document.documentElement.style.setProperty("--hu-light-intensity", String(value));
    }
  }

  private handleMouseMove(e: MouseEvent): void {
    this.targetX = (e.clientX / window.innerWidth) * 100;
    this.targetY = (e.clientY / window.innerHeight) * 100;
  }

  private handleDeviceOrientation(e: DeviceOrientationEvent): void {
    const beta = e.beta ?? 0;
    const gamma = e.gamma ?? 0;
    this.targetX = ((gamma + 90) / 180) * 100;
    this.targetY = ((beta + 90) / 180) * 100;
  }

  private tick(): void {
    this.currentX = lerp(this.currentX, this.targetX, LERP_FACTOR);
    this.currentY = lerp(this.currentY, this.targetY, LERP_FACTOR);
    this.applyProperties();
    this.rafId = requestAnimationFrame(this.boundTick);
  }

  private applyProperties(): void {
    const root = document.documentElement;
    root.style.setProperty("--hu-light-x", `${this.currentX}%`);
    root.style.setProperty("--hu-light-y", `${this.currentY}%`);
    root.style.setProperty("--hu-light-intensity", String(this.intensity));
  }

  private setStaticPosition(x: number, y: number): void {
    const root = document.documentElement;
    root.style.setProperty("--hu-light-x", `${x}%`);
    root.style.setProperty("--hu-light-y", `${y}%`);
    root.style.setProperty("--hu-light-intensity", String(this.intensity));
  }
}

export const dynamicLight = new DynamicLight();
