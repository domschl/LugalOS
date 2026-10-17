# LugalOS CAS: Algebra Manual (Simplification, Polynomials, Equations & Matrices)

LugalOS features a self-contained exact Computer Algebra System running in Scheme-like Lisp. All computations use exact rational $\mathbb{Q}$ and Gaussian rational $\mathbb{Q}(i)$ arithmetic with zero floating-point overhead.

## 1. System Startup

To load the CAS from the LugalOS shell:
```lisp
lsh> lisp
lisp> (load "/sd0/cas/cas.lisp")
=> #t
```

## 2. Canonical Simplification (simplify, expand)

Reduces expressions to irreducible canonical forms, combines like terms, folds constants, and distributes products.

### Signatures
* `(simplify expr)`: Reduces `expr` to irreducible canonical form.
* `(expand expr)`: Distributes multiplications over additions recursively.
* `(cas-real-part expr)`: Real part $\text{Re}(z)$.
* `(cas-imag-part expr)`: Imaginary part $\text{Im}(z)$.
* `(cas-conjugate expr)`: Complex conjugate $a - bi$.
* `(cas-complex-abs expr)`: Exact complex absolute value $|z| = \sqrt{a^2 + b^2}$.

### Examples
```lisp
(simplify '(+ (* 3 x) (* 2 x) 5 -2))
;; => (+ 3 (* 5 x))

(expand '(* (+ x 1) (+ x -1)))
;; => (+ -1 (^ x 2))

(simplify '(sqrt 72))
;; => (* 6 (sqrt 2))

(simplify '(sqrt -4))
;; => (* 2 i)
```

## 3. Polynomial Algebra & Factorization (factor, roots, poly-div)

Performs polynomial arithmetic, division with quotient/remainder, GCD computation, and factorization over $\mathbb{Q}$ and $\mathbb{C}$.

### Signatures
* `(poly-div p q var)`: Returns quotient and remainder `(q r)`.
* `(poly-gcd p q var)`: Greatest common divisor of polynomials.
* `(factor poly var)`: Factors polynomial into irreducible factors over $\mathbb{Q}$.
* `(roots poly var)`: Computes exact roots of polynomial.

### Examples
```lisp
;; Polynomial division: (x^3 - 1) / (x - 1) = x^2 + x + 1
(poly-div '(+ (^ x 3) -1) '(+ x -1) 'x)
;; => ((+ 1 x (^ x 2)) 0)

;; Polynomial factorization
(factor '(+ (^ x 2) -4) 'x)
;; => (* (+ x -2) (+ x 2))

;; Finding polynomial roots
(roots '(+ (^ x 2) -9) 'x)
;; => (-3 3)
```

## 4. Exact Equation Solving (solve)

Solves linear equations, quadratic equations, cubic equations, and systems of simultaneous linear equations in exact rationals or radicals.

### Signatures
* `(solve eq var)`: Solves `eq` for `var`. If `eq` is an expression, solves `eq = 0`.
* `(solve-system eq-list var-list)`: Solves a system of equations for variables.

### Examples
```lisp
;; Linear equation: 3x - 12 = 0
(solve '(= (* 3 x) 12) 'x)
;; => (4)

;; Quadratic equation: x^2 - 5x + 6 = 0
(solve '(= (+ (^ x 2) (* -5 x) 6) 0) 'x)
;; => (2 3)

;; Quadratic with complex roots: x^2 + 4 = 0
(solve '(= (+ (^ x 2) 4) 0) 'x)
;; => ((* -2 i) (* 2 i))

;; System of linear equations: { 2x + y = 7, x - y = 2 }
(solve-system '((= (+ (* 2 x) y) 7) (= (+ x (* -1 y)) 2)) '(x y))
;; => ((x . 3) (y . 1))
```

## 5. Partial Fraction Decomposition (apart)

Decomposes rational functions $\frac{P(x)}{Q(x)}$ into sums of simpler partial fractions.

### Signatures
* `(apart expr var)`: Decomposes rational function `expr` with respect to `var`.

### Examples
```lisp
;; 1 / (x^2 - 1) = 1/(2(x-1)) - 1/(2(x+1))
(apart '(/ 1 (+ (^ x 2) -1)) 'x)
;; => (+ (* 1/2 (/ 1 (+ x -1))) (* -1/2 (/ 1 (+ x 1))))
```

## 6. Exact Linear Algebra (matrix-mul, matrix-det, matrix-inv)

Matrices are represented as lists of row lists `((a11 a12) (a21 a22))`.

### Signatures
* `(matrix-mul A B)`: Matrix multiplication.
* `(matrix-det A)`: Exact determinant via Bareiss fraction-free algorithm.
* `(matrix-inv A)`: Exact matrix inverse via Gauss-Jordan elimination.
* `(matrix-solve A b)`: Solves linear system $A x = b$.

### Examples
```lisp
(define A '((1 2) (3 4)))
(matrix-det A)
;; => -2

(matrix-inv A)
;; => ((-2 1) (3/2 -1/2))
```

## 7. Natural Infix Calculator Mode (calc)

Allows entering mathematical expressions in traditional infix notation:

```lisp
(calc "3*x + 5*x - 2")
;; => (+ -2 (* 8 x))

(calc "solve(x^2 - 4 = 0, x)")
;; => (-2 2)
```

## 8. Algebra Quick Reference Summary

```
| Function | Signature | Description |
| :--- | :--- | :--- |
| simplify | (simplify expr) | Canonical simplification |
| expand | (expand expr) | Distribute multiplications |
| factor | (factor poly var) | Factor polynomial |
| roots | (roots poly var) | Find exact roots |
| solve | (solve eq var) | Solve equation for var |
| solve-system | (solve-system eqs vars) | Solve linear system |
| apart | (apart expr var) | Partial fraction decomposition |
| matrix-det | (matrix-det A) | Matrix determinant |
| matrix-inv | (matrix-inv A) | Matrix inverse |
| matrix-solve | (matrix-solve A b) | Solve A*x = b |
| calc | (calc "infix string") | Evaluate infix expression |
```
