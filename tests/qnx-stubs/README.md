# QNX API stubs (type-check only)

These headers declare the handful of QNX Neutrino calls used by
`src/transport_qnx.c`, with the signatures given in the QNX SDP 8.0 library
reference. `make qnx-typecheck` compiles that file against them on Linux so
CI catches typos and obvious type mistakes.

They are **not** QNX headers and nothing here links or runs. A clean
type-check here proves nothing about how the code behaves on QNX. The real
check is building with `qcc` from the SDP (see `docs/QNX.md`).
