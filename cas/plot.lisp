;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 43.7
;;; Mathematical Plotting on LugalOS Canvas
;;; ============================================================================
;;;
;;; Provides:
;;;   (plot expr (var min max) [opts])       - 2D function plotter with auto-scaling
;;;   (plot-diff expr (var min max) [opts])  - Plots f(x) and symbolic f'(x)
;;;   (plot-multi exprs (var min max) [opts])- Plots multiple functions on shared axes
;;;   (plot-parametric fx fy (t min max))    - Parametric curve plotter
;;;
;;; Uses 100% exact rational & fixed-point arithmetic (zero floating-point in M-mode).
;;;

;;; --- 1. Expression Evaluation for Plotting ---

(define (plot-eval-const sym)
  (cond ((eq? sym 'pi) 3141592/1000000)
        ((eq? sym 'e)  2718281/1000000)
        (else #f)))

(define (plot-sqrt x)
  (cond ((< x 0) #f)
        ((= x 0) 0)
        (else
         (let* ((p (numerator x))
                (q (denominator x))
                ;; floor(sqrt(p*q * 10^12)) / (q * 10^6)
                (scaled (quotient (* p 1000000000000) q))
                (s (isqrt scaled)))
           (/ s 1000000)))))

;; Taylor expansion for exp(x) around 0: 1 + x + x^2/2 + x^3/6 + ... + x^7/5040
(define (plot-exp x)
  (cond ((> x 8) #f)
        ((< x -8) 0)
        (else
         (let loop ((term 1) (sum 1) (n 1))
           (if (> n 7)
               sum
               (let* ((next-term (/ (* term x) n))
                      (next-sum (+ sum next-term)))
                 (loop next-term next-sum (+ n 1))))))))

;; Natural logarithm for plotting: ln(x) for x > 0 using argument reduction
(define (plot-ln x)
  (cond ((<= x 0) #f)
        ((= x 1) 0)
        (else
         ;; Reduce x into [1/2, 2] by factors of 2 (ln 2 ~ 693147/1000000)
         (let loop ((val x) (k 0))
           (cond ((> val 2)   (loop (/ val 2) (+ k 1)))
                 ((< val 1/2) (loop (* val 2) (- k 1)))
                 (else
                  ;; Series for ln((1+z)/(1-z)) with z = (val - 1) / (val + 1)
                  (let* ((z (/ (- val 1) (+ val 1)))
                         (z2 (* z z))
                         (term1 z)
                         (term2 (/ (* term1 z2) 3))
                         (term3 (/ (* term2 z2 3) 5))
                         (term4 (/ (* term3 z2 5) 7))
                         (ln-val (* 2 (+ term1 term2 term3 term4)))
                         (ln2 693147/1000000))
                    (+ ln-val (* k ln2)))))))))

;; Evaluate an expression E with var = val (val is a number)
(define (plot-eval expr var val)
  (cond
    ((number? expr) expr)
    ((symbol? expr)
     (cond ((eq? expr var) val)
           (else
            (let ((c (plot-eval-const expr)))
              (if c c #f)))))
    ((procedure? expr)
     (expr val))
    ((string? expr)
     (plot-eval (from-infix expr) var val))
    ((pair? expr)
     (let ((op (car expr))
           (args (cdr expr)))
       (cond
         ((eq? op '+)
          (let loop ((as args) (acc 0))
            (if (null? as)
                acc
                (let ((v (plot-eval (car as) var val)))
                  (if (not v) #f (loop (cdr as) (+ acc v)))))))
         ((eq? op '-)
          (cond ((null? args) 0)
                ((null? (cdr args))
                 (let ((v (plot-eval (car args) var val)))
                   (if (not v) #f (- v))))
                (else
                 (let ((first (plot-eval (car args) var val)))
                   (if (not first) #f
                       (let loop ((as (cdr args)) (acc first))
                         (if (null? as)
                             acc
                             (let ((v (plot-eval (car as) var val)))
                               (if (not v) #f (loop (cdr as) (- acc v)))))))))))
         ((eq? op '*)
          (let loop ((as args) (acc 1))
            (if (null? as)
                acc
                (let ((v (plot-eval (car as) var val)))
                  (if (not v) #f (loop (cdr as) (* acc v)))))))
         ((eq? op '/)
          (if (not (= (length args) 2)) #f
              (let ((num (plot-eval (car args) var val))
                    (den (plot-eval (cadr args) var val)))
                (if (or (not num) (not den) (= den 0)) #f (/ num den)))))
         ((or (eq? op '^) (eq? op 'expt))
          (if (not (= (length args) 2)) #f
              (let ((b (plot-eval (car args) var val))
                    (p (plot-eval (cadr args) var val)))
                (cond ((or (not b) (not p)) #f)
                      ((= p 0) 1)
                      ((and (integer? p) (> p 0)) (expt b p))
                      ((and (integer? p) (< p 0))
                       (if (= b 0) #f (/ 1 (expt b (- p)))))
                      ((equal? p 1/2) (plot-sqrt b))
                      ((equal? p -1/2)
                       (let ((s (plot-sqrt b)))
                         (if (or (not s) (= s 0)) #f (/ 1 s))))
                      (else #f)))))
         ((eq? op 'sqrt)
          (if (not (= (length args) 1)) #f
              (let ((v (plot-eval (car args) var val)))
                (if (not v) #f (plot-sqrt v)))))
         ((eq? op 'sin)
          (if (not (= (length args) 1)) #f
              (let ((v (plot-eval (car args) var val)))
                (if (not v) #f (math-sin v)))))
         ((eq? op 'cos)
          (if (not (= (length args) 1)) #f
              (let ((v (plot-eval (car args) var val)))
                (if (not v) #f (math-cos v)))))
         ((eq? op 'tan)
          (if (not (= (length args) 1)) #f
              (let* ((v (plot-eval (car args) var val))
                     (c (if v (math-cos v) #f)))
                (if (or (not c) (= c 0)) #f
                    (/ (math-sin v) c)))))
         ((eq? op 'exp)
          (if (not (= (length args) 1)) #f
              (let ((v (plot-eval (car args) var val)))
                (if (not v) #f (plot-exp v)))))
         ((or (eq? op 'log) (eq? op 'ln))
          (if (not (= (length args) 1)) #f
              (let ((v (plot-eval (car args) var val)))
                (if (not v) #f (plot-ln v)))))
         ((eq? op 'abs)
          (if (not (= (length args) 1)) #f
              (let ((v (plot-eval (car args) var val)))
                (if (not v) #f (if (< v 0) (- v) v)))))
         (else #f))))
    (else #f)))

;;; --- 2. Sampling and Auto-Scaling ---

;; Sample an expression over [xmin, xmax] with n intervals (n+1 points)
(define (plot-sample-fn expr var xmin xmax n)
  (let* ((ast (if (string? expr) (from-infix expr) expr))
         (dx (/ (- xmax xmin) n)))
    (let loop ((i 0) (pts '()))
      (if (> i n)
          (reverse pts)
          (let* ((x (+ xmin (* i dx)))
                 (y (plot-eval ast var x)))
            (loop (+ i 1) (cons (cons x y) pts)))))))

;; Auto-scale Y range across multiple sampled point lists
(define (plot-find-bounds pts-list)
  (let loop-lists ((ls pts-list) (min-y #f) (max-y #f))
    (if (null? ls)
        (cond
          ((not min-y) (cons -1 1))
          ((= min-y max-y) (cons (- min-y 1) (+ max-y 1)))
          (else
           ;; Add 5% margin padding above and below
           (let* ((span (- max-y min-y))
                  (pad (/ span 20))
                  (pad (if (= pad 0) 1/10 pad)))
             (cons (- min-y pad) (+ max-y pad)))))
        (let loop-pts ((pts (car ls)) (cur-min min-y) (cur-max max-y))
          (if (null? pts)
              (loop-lists (cdr ls) cur-min cur-max)
              (let ((y (cdar pts)))
                (if (or (not y) (not (number? y)))
                    (loop-pts (cdr pts) cur-min cur-max)
                    (let ((new-min (if (or (not cur-min) (< y cur-min)) y cur-min))
                          (new-max (if (or (not cur-max) (> y cur-max)) y cur-max)))
                      (loop-pts (cdr pts) new-min new-max)))))))))

;;; --- 3. Rendering to Canvas ---

(define (plot-opt-get opts key dflt)
  (let ((entry (assq key opts)))
    (if entry (cdr entry) dflt)))

(define (plot-floor r)
  (if (integer? r) r (quotient (numerator r) (denominator r))))

(define (plot-draw-scene title xmin xmax ymin ymax curves)
  (let* ((size (canvas-size))
         (w (if (> (car size) 0) (car size) 400))
         (h (if (> (cadr size) 0) (cadr size) 300))
         (margin-l 44)
         (margin-r 16)
         (margin-t 24)
         (margin-b 24)
         (pw (- w margin-l margin-r))
         (ph (- h margin-t margin-b)))
    (if (or (<= pw 20) (<= ph 20))
        #f
        (begin
          ;; 1. Canvas setup
          (if title (canvas-title title))
          (canvas-fill 0)

          ;; 2. Frame border around plotting viewport
          (canvas-frame margin-l margin-t pw ph 1)

          ;; 3. Coordinate mapping functions
          (let* ((x-span (- xmax xmin))
                 (y-span (- ymax ymin))
                 (map-x (lambda (x)
                          (+ margin-l (plot-floor (/ (* (- x xmin) pw) x-span)))))
                 (map-y (lambda (y)
                          (+ margin-t (- ph (plot-floor (/ (* (- y ymin) ph) y-span)))))))

            ;; 4. Axes (if in view)
            ;; Horizontal axis (y = 0)
            (if (and (<= ymin 0) (<= 0 ymax))
                (let ((y0 (map-y 0)))
                  (canvas-line margin-l y0 (+ margin-l pw) y0 1)))

            ;; Vertical axis (x = 0)
            (if (and (<= xmin 0) (<= 0 xmax))
                (let ((x0 (map-x 0)))
                  (canvas-line x0 margin-t x0 (+ margin-t ph) 1)))

            ;; 5. Labels & ticks
            ;; X min / max labels
            (canvas-text margin-l (+ margin-t ph 4) (to-decimal xmin 1))
            (canvas-text (max margin-l (- (+ margin-l pw) 32))
                         (+ margin-t ph 4)
                         (to-decimal xmax 1))

            ;; Y min / max labels
            (canvas-text 2 margin-t (to-decimal ymax 1))
            (canvas-text 2 (max margin-t (- (+ margin-t ph) 10)) (to-decimal ymin 1))

            ;; 6. Render curve segments
            (for-each
             (lambda (curve)
               (let ((pts (car curve))
                     (color (cdr curve)))
                 (let loop ((ps pts) (last-px #f) (last-py #f))
                   (if (pair? ps)
                       (let* ((pt (car ps))
                              (x (car pt))
                              (y (cdr pt)))
                         (if (and y (number? y))
                             (let ((px (map-x x))
                                   (py (map-y y)))
                               (if (and last-px last-py)
                                   (let ((c-py1 (max margin-t (min (+ margin-t ph) last-py)))
                                         (c-py2 (max margin-t (min (+ margin-t ph) py))))
                                     (canvas-line last-px c-py1 px c-py2 color)))
                               (loop (cdr ps) px py))
                             ;; Discontinuity / asymptote: reset segment
                             (loop (cdr ps) #f #f)))))))
             curves)
            #t)))))

;;; --- 4. High-Level User Plotting Functions ---

(define (plot-parse-domain dom)
  (cond ((and (pair? dom) (pair? (cdr dom)) (pair? (cddr dom)))
         (list (car dom) (cadr dom) (caddr dom)))
        ((symbol? dom)
         (list dom -5 5))
        (else '(x -5 5))))

;; (plot expr (var min max) [opts])
(define (plot expr domain . rest-opts)
  (let* ((opts (if (pair? rest-opts) (car rest-opts) '()))
         (dom (plot-parse-domain domain))
         (var (car dom))
         (xmin (cadr dom))
         (xmax (caddr dom))
         (n (plot-opt-get opts 'samples 40))
         (pts (plot-sample-fn expr var xmin xmax n))
         (bounds (plot-find-bounds (list pts)))
         (ymin (plot-opt-get opts 'ymin (car bounds)))
         (ymax (plot-opt-get opts 'ymax (cdr bounds)))
         (title-dflt (cond ((string? expr) expr)
                           ((pair? expr) (to-infix expr))
                           (else "Plot")))
         (title (plot-opt-get opts 'title title-dflt))
         (color (plot-opt-get opts 'color 1))
         (curves (list (cons pts color))))
    (canvas-window 'split)
    (canvas-on-redraw (lambda () (plot-draw-scene title xmin xmax ymin ymax curves)))
    (plot-draw-scene title xmin xmax ymin ymax curves)))

;; (plot-diff expr (var min max) [opts])
;; Plots f(x) and its symbolic derivative f'(x) on the same axes!
(define (plot-diff expr domain . rest-opts)
  (let* ((opts (if (pair? rest-opts) (car rest-opts) '()))
         (dom (plot-parse-domain domain))
         (var (car dom))
         (xmin (cadr dom))
         (xmax (caddr dom))
         (n (plot-opt-get opts 'samples 40))
         (ast (if (string? expr) (from-infix expr) expr))
         (d-ast (simplify (diff ast var)))
         (pts-f (plot-sample-fn ast var xmin xmax n))
         (pts-df (plot-sample-fn d-ast var xmin xmax n))
         (bounds (plot-find-bounds (list pts-f pts-df)))
         (ymin (plot-opt-get opts 'ymin (car bounds)))
         (ymax (plot-opt-get opts 'ymax (cdr bounds)))
         (title-dflt (string-append (to-infix ast) " & (" (to-infix d-ast) ")'"))
         (title (plot-opt-get opts 'title title-dflt))
         (curves (list (cons pts-f 1) (cons pts-df 1))))
    (canvas-window 'split)
    (canvas-on-redraw (lambda () (plot-draw-scene title xmin xmax ymin ymax curves)))
    (plot-draw-scene title xmin xmax ymin ymax curves)))

;; (plot-multi expr-list (var min max) [opts])
(define (plot-multi exprs domain . rest-opts)
  (let* ((opts (if (pair? rest-opts) (car rest-opts) '()))
         (dom (plot-parse-domain domain))
         (var (car dom))
         (xmin (cadr dom))
         (xmax (caddr dom))
         (n (plot-opt-get opts 'samples 40))
         (pts-list (map (lambda (e) (plot-sample-fn e var xmin xmax n)) exprs))
         (bounds (plot-find-bounds pts-list))
         (ymin (plot-opt-get opts 'ymin (car bounds)))
         (ymax (plot-opt-get opts 'ymax (cdr bounds)))
         (title (plot-opt-get opts 'title "Multi Plot"))
         (curves (map (lambda (pts) (cons pts 1)) pts-list)))
    (canvas-window 'split)
    (canvas-on-redraw (lambda () (plot-draw-scene title xmin xmax ymin ymax curves)))
    (plot-draw-scene title xmin xmax ymin ymax curves)))

;; (plot-parametric x-expr y-expr (t min max) [opts])
(define (plot-parametric x-expr y-expr t-domain . rest-opts)
  (let* ((opts (if (pair? rest-opts) (car rest-opts) '()))
         (x-ast (if (string? x-expr) (from-infix x-expr) x-expr))
         (y-ast (if (string? y-expr) (from-infix y-expr) y-expr))
         (dom (plot-parse-domain t-domain))
         (t-var (car dom))
         (tmin (cadr dom))
         (tmax (caddr dom))
         (n (plot-opt-get opts 'samples 50))
         (dt (/ (- tmax tmin) n))
         (pts (let loop ((i 0) (acc '()))
                (if (> i n)
                    (reverse acc)
                    (let* ((t (+ tmin (* i dt)))
                           (x (plot-eval x-ast t-var t))
                           (y (plot-eval y-ast t-var t)))
                      (loop (+ i 1)
                            (cons (if (and x y (number? x) (number? y))
                                      (cons x y)
                                      (cons #f #f))
                                  acc))))))
         (bounds-x (plot-find-bounds (list (map (lambda (p) (cons 0 (car p))) pts))))
         (bounds-y (plot-find-bounds (list (map (lambda (p) (cons 0 (cdr p))) pts))))
         (xmin (plot-opt-get opts 'xmin (car bounds-x)))
         (xmax (plot-opt-get opts 'xmax (cdr bounds-x)))
         (ymin (plot-opt-get opts 'ymin (car bounds-y)))
         (ymax (plot-opt-get opts 'ymax (cdr bounds-y)))
         (title (plot-opt-get opts 'title "Parametric Plot"))
         (curves (list (cons pts 1))))
    (canvas-window 'split)
    (canvas-on-redraw (lambda () (plot-draw-scene title xmin xmax ymin ymax curves)))
    (plot-draw-scene title xmin xmax ymin ymax curves)))
