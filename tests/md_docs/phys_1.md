# Special Relativity

## Two postulates

- **The principle of _Relativity_** – the laws describing change in physical systems are invariant to uniform translatory motion relative to each other.
- **The principle of invariant _speed of light_** - light is always propagated in empty space with a definite velocity $c$ which is independent of the state of motion of the emitting body. Light in vacuum propagates with the speed $c$ (a fixed constant, independent of direction) in at least one system of inertial coordinates (the "stationary system"), regardless of the state of motion of the light source.

## Time dilation

_Time dilation_ is a phenomenon that shows that a
clock that moves relative to an observer seems to go slower to the observer compared to a stational clock.

In reference system $1$ a light beam travels from $P_0$ to $P_1$, hitting a mirror and back to $P_0$.
The time for the complete journey is measured as $t_0$, and the distance between origin and mirror $L_0$ is $L_0=c\frac{t_0}{2}$. Now in a second reference system $2$ that travels with speed $\textbf{v}$ relative to system $1$, the light beam travels from $P_2$ via $P_1$ to $P_3$ using time $t$, with $L=c\frac{t}{2}$.

It then follows from $c$ being constant in both systems:
$$
(v\frac{t}{2})^2+L_0^2=L^2
$$

### Intermezzo used for length contraction, $t$ as function of $L_0$

$$
(v\frac{t}{2})^2+L_0^2=(c\frac{t}{2})^2
$$
$$
(c\frac{t}{2})^2-(v\frac{t}{2})^2=L_0^2
$$
$$
t^2=\frac{L_0^2}{\frac{c^2}{4}-\frac{v^2}{4}}
$$
$$
t=\frac{\frac{2L_0}{c}}{\sqrt{1-\frac{v^2}{c^2}}}\quad\text{Gl. 1}
$$

### continued

$$
(v\frac{t}{2})^2+(c\frac{t_0}{2})^2=(c\frac{t}{2})^2
$$
$$
(c\frac{t_0}{2})^2=(c\frac{t}{2})^2-(v\frac{t}{2})^2
$$
$$
t_0=t\sqrt{1-\frac{v^2}{c^2}}\quad\text{or}\quad t=\frac{t_0}{\sqrt{1-\frac{v^2}{c^2}}}
$$

### Twin paradox

Two observers A and B. If B moves relative to A, B's clock should move slower compared to A's clock, since B moves. However, from viewpoint of B, also A moves, so the clock of A should move slower compared to B's clock. If A and B meet again, a paradoxon occures.

Solution: time dilation is not symmetrical, if acceleration is involved: in order for B to return to A, acceleration is required.

## Length contraction

An observer moving with velocity $\mathbf{v}$ from $P_0$ to $P_1$ in time $t_1$ and back (total time $t$) measures the distance as $L$. Way from $P_0$ to $P_1$ (in direction of $\mathbf{v}$):

$$
ct_1=L+vt_1\quad\text{with }\quad t_1=\frac{L}{c-v}
$$

Back from $P_1$ to $P_0$:

$$
c(t-t_1)=L-v(t-t_1)
$$

$$
ct-ct_1=L-vt+vt_1
$$
$$
(c+v)t=L+(c+v)t_1
$$
$$
t=\frac{L}{c+v}+t_1=\frac{L}{c+v}+\frac{L}{c-v}=\frac{L(c-v)+L(c+v)}{(c+v)(c-v)}
$$
$$
t=\frac{2Lc}{c^2-v^2}=\frac{2\frac{L}{c}}{1-\frac{v^2}{c^2}}
$$
Using $t$ from time dilation $\text{Gl. 1}$ above:
$$
\frac{2\frac{L}{c}}{1-\frac{v^2}{c^2}}=\frac{\frac{2L_0}{c}}{\sqrt{1-\frac{v^2}{c^2}}}
$$
$$
L=L_0\sqrt{1-\frac{v^2}{c^2}}
$$
## Lorentz Transformation[$^{wp}](https://en.wikipedia.org/wiki/Lorentz_transformation)

The _Lorentz transformation_s are a six-parameter family of linear transformations from a coordinate frame $S$ in [[Space-Time]] to another frame $S'$ that moves at a constant velocity $\mathbf{v}$ relative to the former.

![LorenzTrImg](../Images/CoordinateTranslation.svg)
_A coordinate system $S'$ moves relative to $S$ with velocity $\mathbf{v}$ relative to the $x$ axis._

$$
\begin{array}{ll}
x'=\gamma(x-vt)\quad\text{or}\quad x=\gamma(x'+vt') \\
y'=y \\
z'=z
\end{array}
$$

with $x=\gamma^2(x-vt)+\gamma vt'$ it follows:

$$
t'=\gamma t+\frac{1-\gamma^2}{\gamma v}x
$$

Both systems $S$, $S'$ are at same origin at $t=0$. A light beam then propagates for observers in both system (due to the 2nd postulate) as:

$$
x=ct\quad\text{and}\quad x'=ct'
$$
From $x'=ct'$, rewritten as $x$, $t$:
$$
\begin{array}{ll}
x'=ct' \\
\gamma(x-vt)=\gamma ct+\frac{1-\gamma^2}{\gamma v}cx
\end{array}
$$
$$
x=\frac{\gamma ct+\gamma vt}{\gamma-(\frac{1-\gamma^2}{\gamma v})c}
$$
with $x=ct$:
$$
ct=ct\left[\frac{\gamma+\gamma\frac{v}{c}}{\gamma-(\frac{1-\gamma^2}{\gamma v})c}\right]
$$
so:
$$
\gamma=\frac{1}{\sqrt{1-\frac{v^2}{c^2}}}
$$
So the **Lorentz transformation** results in:
$$
\begin{array}{ll}
x'=\frac{x-vt}{\sqrt{1-\frac{v^2}{c^2}}} \quad &= \gamma(x-vt)\\
y'=y \\
z'=z \\
t'=\frac{t-\frac{vx}{c^2}}{\sqrt{1-\frac{v^2}{c^2}}} \quad &=\gamma(t-\frac{vx}{c^2})
\end{array}
$$

### Length contraction

From this, the **length contraction** can be derived with $x_1'$ at $P_0$ and $x_2'$ at $P_1$:
$$
L_0=x_2'-x_1'=\frac{x_1-vt}{\sqrt{1-\frac{v^2}{c^2}}}-\frac{x_2-vt}{\sqrt{1-\frac{v^2}{c^2}}}
$$
with $L=x_1-x_2$ follows:
$$
L=L_0\sqrt{1-\frac{v^2}{c^2}}
$$
### Time dilation

The **inverse lorentz transformation** is obtained by changing $x_i'\leftrightarrow x_i$ and $v\leftrightarrow -v$. It can be used to derive the **time dilation**:

$$
\begin{array}{ll}
x=\frac{x'+vt'}{\sqrt{1-\frac{v^2}{c^2}}} \\
y=y' \\
z=z' \\
t=\frac{t'+\frac{vx'}{c^2}}{\sqrt{1-\frac{v^2}{c^2}}}
\end{array}
$$
An observer in system $S'$ measures $t_1'$ and after $t_0$ measures $t_2'$ with $t_0=t_2'-t_1'$. An observer in system $S$ measures:
$$
t=t_2-t_1=\frac{t_2'+\frac{vx'}{c^2}}{\sqrt{1-\frac{v^2}{c^2}}}-\frac{t_1'+\frac{vx'}{c^2}}{\sqrt{1-\frac{v^2}{c^2}}}
$$
so:
$$
t=\frac{t_0}{\sqrt{1-\frac{v^2}{c^2}}}
$$

## Relativistic mass

Again, reference system $S'$ moves relative to system $S$ with speed $\mathbf{v}$ in direction of the $x$-axis.
