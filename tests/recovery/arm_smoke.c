/* Synthetic toolchain check only. This is NOT kotoGBA or a B19 build. */
unsigned kotogba_infrastructure_smoke(unsigned value) {
    return (value ^ 0x4b4f544fu) + 1u;
}
