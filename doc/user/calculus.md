# LugalOS CAS: Calculus Manual (Differentiation, Integration, Limits & Series)

LugalOS features a self-contained exact Computer Algebra System running in Scheme-like Lisp. All computations use exact rational $\mathbb{Q}$ and Gaussian rational $\mathbb{Q}(i)$ arithmetic with zero floating-point overhead.

## 1. System Startup

To load the CAS from the LugalOS shell:
```lisp
lsh> lisp
lisp> (load "/sd0/cas/cas.lisp")
=> #t
```

## 2. Symbolic Differentiation (diff, derivative)

Computes the formal derivative $\frac{d^n}{dx^n} f(x)$ using differential algebra rules (sum, product, quotient, and chain rules).

### Signatures
* `(diff expr var)`: First derivative with respect to `var`.
* `(diff expr var order)`: n-th order derivative with respect to `var`.
* `(derivative expr var)`: Alias for `diff`.

### Supported Functions
`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `exp`, `log` ($\ln$), `sqrt`, `abs`, and polynomial powers `^`.

### Examples
```lisp
;; First derivative: d/dx [sin(2x)] = 2*cos(2x)
(diff '(sin (* 2 x)) 'x)
;; => (* 2 (cos (* 2 x)))

;; Second derivative: d^2/dx^2 [sin(2x)] = -4*sin(2x)
(diff '(sin (* 2 x)) 'x 2)
;; => (* -4 (sin (* 2 x)))

;; Product and chain rules: d/dx [x^2 * e^x]
(diff '(* (^ x 2) (exp x)) 'x)
;; => (+ (* 2 x (exp x)) (* (^ x 2) (exp x)))

;; Hyperbolic derivatives: d/dx [tanh(x)] = 1 - tanh^2(x)
(diff '(tanh x) 'x)
;; => (+ 1 (* -1 (^ (tanh x) 2)))
```

### Visualizing Derivatives (`plot-diff`)
Graph a function and its exact symbolic derivative simultaneously on shared coordinate axes:
```lisp
(load "/sd0/cas/plot.lisp")
(plot-diff "x*sin(x)" '(x -6 6) '((title . "f(x) & df/dx") (samples . 60)))
```

## 3. Symbolic Integration (integrate, integrate-def)

Computes indefinite and definite integrals using exact symbolic techniques, partial fraction decomposition (`apart`), and integration by parts.

### Signatures
* `(integrate expr var)`: Indefinite integral $\int f(x) dx$ (omitting arbitrary constant + C).
* `(integrate-def expr var a b)`: Definite integral $\int_a^b f(x) dx$ via the Fundamental Theorem of Calculus $F(b) - F(a)$.

### Examples
```lisp
;; Integration by parts: \int x * e^x dx = e^x * (x - 1)
(integrate '(* x (exp x)) 'x)
;; => (* (exp x) (+ -1 x))

;; Rational integration: \int 1 / (x^2 + 1) dx = atan(x)
(integrate '(/ 1 (+ (^ x 2) 1)) 'x)
;; => (atan x)

;; Definite integral: \int_0^1 x^2 dx = 1/3
(integrate-def '(^ x 2) 'x 0 1)
;; => 1/3
```

## 4. Symbolic Limits (limit, limes)

Computes exact limits $\lim_{x \to x_0} f(x)$ using algebraic simplification, synthetic division of removable singularities, and L'Hopital's rule.

### Signatures
* `(limit expr var point)`: Computes $\lim_{var \to point} expr$.
* `(limes expr var point)`: Alias for `limit`.

### Examples
```lisp
;; Removable singularity: \lim_{x \to 1} (x^2 - 1) / (x - 1) = 2
(limit '(/ (+ (^ x 2) -1) (+ x -1)) 'x 1)
;; => 2

;; Trigonometric limit: \lim_{x \to 0} sin(x) / x = 1
(limit '(/ (sin x) x) 'x 0)
;; => 1
```

## 5. Taylor & Maclaurin Series (taylor)

Computes the n-th order polynomial Taylor expansion about $x = x_0$:
$T_n(x) = \sum_{k=0}^n \frac{f^{(k)}(x_0)}{k!} (x - x_0)^k$

### Signatures
* `(taylor expr var point order)`: Computes order `order` Taylor polynomial about `point`.

### Examples
```lisp
;; 5th-order Maclaurin series of sin(x): x - x^3/6 + x^5/120
(taylor '(sin x) 'x 0 5)
;; => (+ x (* -1/6 (^ x 3)) (* 1/120 (^ x 5)))

;; 4th-order expansion of e^x: 1 + x + x^2/2 + x^3/6 + x^4/24
(taylor '(exp x) 'x 0 4)
;; => (+ 1 x (* 1/2 (^ x 2)) (* 1/6 (^ x 3)) (* 1/24 (^ x 4)))
```

## 6. Trigonometric & Exponential Transformations

Transforms trigonometric expressions into polynomials or complex exponentials.

### Signatures
* `(trig-expand expr)`: Expands multiple angles and angle additions into powers.
* `(trig-reduce expr)`: Linearizes products and powers into single-frequency sums.
* `(to-exponential expr)`: Rewrites trigonometric and hyperbolic functions as complex exponentials $e^{ix}$.

### Examples
```lisp
(trig-expand '(sin (* 2 x)))
;; => (* 2 (cos x) (sin x))

(trig-reduce '(* (cos x) (sin x)))
;; => (* 1/2 (sin (* 2 x)))
```

## 7. Calculus Quick Reference Summary

```
| Function | Signature | Description |
| :--- | :--- | :--- |
| diff | (diff expr var [order]) | Symbolic derivative d^n/dx^n |
| integrate | (integrate expr var) | Indefinite integral |
| integrate-def | (integrate-def expr var a b)| Definite integral over [a, b] |
| limit | (limit expr var point) | Limit as var -> point |
| taylor | (taylor expr var x0 order) | Taylor series expansion |
| trig-expand | (trig-expand expr) | Expand trig sums & multiples |
| trig-reduce | (trig-reduce expr) | Reduce trig products to sums |
| to-exponential| (to-exponential expr) | Convert trig to complex exp |
| plot-diff | (plot-diff expr range opts) | Plot curve and its derivative |
```
