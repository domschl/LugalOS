# General relativity

## Einstein's field equations

### Componts of _Einstein field equation_

$$
R_{\mu\nu}-\frac{1}{2}Rg_{\mu\nu}+\Lambda g_{\mu\nu}=\frac{8\pi G}{c^4}T_{\mu\nu}
$$

$R_{\mu\nu} \rightarrow$ Ricci curvature tensor
$R \rightarrow$ Ricci curvature scalar
$g_{\mu\nu} \rightarrow$ Metric tensor
$\Lambda\rightarrow$ _Cosmological constant_
$G\rightarrow$ _Gravitational constant_
$c \rightarrow$ _Speed of light_
$T_{\mu\nu}\rightarrow$ _stress energy momentum tensor_

$\mu\nu$ denotes space-time in four dimensions, with $0\rightarrow$ time, $1\rightarrow x$, $2\rightarrow y$, $3\rightarrow z$.

16 partial differential equations: $R{\mu\nu}\rightarrow 4$, $g{\mu\nu}\rightarrow 4$, $g{\mu\nu}\rightarrow 4$, $T{\mu\nu}\rightarrow 4$.
Six equations are duplicate, which makes a total of 10 non linear partial differential equations.
$g_{\mu\nu}$ is a symmetric tensor with $g_{\mu\nu}=g_{\nu\mu}$, a metric tensor with 10 unique components:

$$
g_{\mu\nu}=\begin{pmatrix}
g_{00} & g_{01} & g_{02} & g_{03} \\
 & g_{11} & g_{12} & g_{13} \\
& & g_{22} & g_{23} \\
& & & g_{33}
\end{pmatrix}
$$

$T_{\mu\nu}$ is a symmetric tensor too with 10 unique components.

The left side of the field equation $R_{\mu\nu}-\frac{1}{2}Rg_{\mu\nu}+\Lambda g_{\mu\nu}$ shows the curvature of space-time as determined by the metric '$g$', the geometry of space-time.
The right sie of the field equation $\frac{8\pi G}{c^4}T_{\mu\nu}$ is the matter energy content of space-time. The entire matter of the universe and it's movement is denoted by this.
So:

- _Geometry of space-time $=$ Matter movement_

### What is a stress-energy-momentum tensor?

- _Curvature $=$ matter_

The presence of matter curves space-time, so what consists of matter?
From _Special Relativity_:

$$
E=m_{\text{rel}}c^2\quad\rightarrow\quad\text{Relativistic mass}
$$

$$
E=\sqrt{(mc^2)^2+(pc)^2}\quad\rightarrow\quad\text{Total energy of a moving body}
$$

Matter is formed by:

- energy
- density
- momentum
- flow of energy
- pressure
- stress
(Those are not the classical definitions). E.g.:
The energy momentum tensor, in physics, describes the local distribution of mass, momentum, and energy.

Stress energy momentum tensor:

$$
T_{\mu\nu}=\begin{pmatrix}
T_{00} & T_{01} & T_{02} & T_{03} \\
T_{10} & T_{11} & T_{12} & T_{13} \\
T_{20} & T_{21} & T_{22} & T_{23} \\
T_{30} & T_{31} & T_{32} & T_{33}
\end{pmatrix}
$$

with:

- $T_{00}$ Energy density
- $T_{i0}$, $T_{0i}\text{ with }i\in[1,2,3]$ Momentum density
- $T_{ii}\text{ with }i\in[1,2,3]$ Pressure
- $T_{12}$, $T_{13}$, $T_{23}$ Shear stress
- $T_{21}$, $T_{31}$, $T_{32}$ Momentum flux

$$
T_{\mu\nu}=\begin{pmatrix}
\text{energy}\atop\text{density} & \text{momentum}\atop\text{density} & \text{momentum}\atop\text{density} & \text{momentum}\atop\text{density} \\
\text{momentum}\atop\text{density} & \text{pressure}\atop & \text{shear}\atop\text{stress} & \text{shear}\atop\text{stress} \\
\text{momentum}\atop\text{density} & \text{momentum}\atop\text{flux} & \text{pressure}\atop & \text{shear}\atop\text{stress} \\
\text{momentum}\atop\text{density} & \text{momentum}\atop\text{flux} & \text{momentum}\atop\text{flux} & \text{pressure}\atop
\end{pmatrix}
$$

- **Energy density**: the amount of energy stored in a given system or region of space per unit volume. It therefore has the units of energy per length cubed.
- **Energy flux**: the rate of transfer of energy through a surface.
- **Momentum density**: The momentum per unit volume of an given field.
- **Momentum flux**: the rate of change of momentum flowing through a unit area.
- **Pressure**: $P=FA$
- **Shear stress**: $\tau=\frac{F}{A}$

It is energy, not mass, that is the source of gravity. $T_{\mu\nu}\rightarrow$ zero divergence:

$$
0 = \operatorname{div} T = T^{\mu \nu}{}_{;\nu} = \nabla_{\nu} T^{\mu \nu} = T^{\mu \nu}{}_{,\nu} +  \Gamma^{\mu}{}_{\sigma \nu}T^{\sigma \nu} + \Gamma^{\nu}{}_{\sigma \nu} T^{\mu \sigma}
$$

## Ricci curvature

$$
R_{ab}=R_{abc}^c
$$

The Ricci curvature tensor measures how a shape is deformed as it moves along the geodesics in the space.
