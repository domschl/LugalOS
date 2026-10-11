;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 43.4
;;; Symbolic Differentiation & Integration
;;; ============================================================================
;;;
;;; Provides:
;;;   (diff expr var [n])          - Symbolic n-th order derivative d^n/dvar^n
;;;   (integrate expr var)         - Indefinite integral with respect to var
;;;   (integrate-def expr var a b) - Definite integral: F(b) - F(a)
;;;   (taylor expr var order [x0]) - Taylor / Maclaurin series expansion
;;;

;;; --- 1. Variable Occurrence & Pattern Helpers ---

(define (cas-has-var? expr var)
  (cond ((eq? expr var) #t)
        ((pair? expr)
         (or (cas-has-var? (car expr) var)
             (cas-has-var? (cdr expr) var)))
        (else #f)))

(define (cas-has-op? expr op)
  (cond ((not (pair? expr)) #f)
        ((eq? (car expr) op) #t)
        (else
         (let loop ((rest (cdr expr)))
           (cond ((null? rest) #f)
                 ((cas-has-op? (car rest) op) #t)
                 (else (loop (cdr rest))))))))

(define (cas-subst expr var val)
  (cond ((equal? expr var) val)
        ((pair? expr)
         (cons (car expr) (map (lambda (x) (cas-subst x var val)) (cdr expr))))
        (else expr)))

;; Matches expr to linear form (a * var + b).
;; Returns (cons a b) if linear in var (with a != 0), or #f otherwise.
(define (cas-match-linear expr var)
  (cond
    ((eq? expr var)
     (cons 1 0))
    ((not (cas-has-var? expr var))
     #f)
    ((and (pair? expr) (eq? (car expr) '*))
     (let loop ((rest (cdr expr)) (c-acc '()) (v-term #f))
       (if (null? rest)
           (if (and v-term (eq? v-term var))
               (cons (if (null? c-acc) 1 (simplify (cons '* (reverse c-acc))))
                     0)
               #f)
           (if (cas-has-var? (car rest) var)
               (if v-term
                   #f
                   (loop (cdr rest) c-acc (car rest)))
               (loop (cdr rest) (cons (car rest) c-acc) v-term)))))
    ((and (pair? expr) (eq? (car expr) '+))
     (let loop ((rest (cdr expr)) (c-terms '()) (v-term #f))
       (if (null? rest)
           (if v-term
               (let ((sub-lin (cas-match-linear v-term var)))
                 (if sub-lin
                     (cons (car sub-lin)
                           (simplify (cons '+ (cons (cdr sub-lin) c-terms))))
                     #f))
               #f)
           (if (cas-has-var? (car rest) var)
               (if v-term
                   #f
                   (loop (cdr rest) c-terms (car rest)))
               (loop (cdr rest) (cons (car rest) c-terms) v-term)))))
    (else #f)))

;;; --- 2. Symbolic Differentiation (diff) ---

(define (cas-diff-mul factors var)
  (cond ((null? factors) 0)
        ((null? (cdr factors)) (cas-diff-1 (car factors) var))
        (else
         (let ((u (car factors))
               (v (if (null? (cddr factors))
                      (cadr factors)
                      (cons '* (cdr factors)))))
           (list '+
                 (list '* (cas-diff-1 u var) v)
                 (list '* u (cas-diff-mul (cdr factors) var)))))))

(define (cas-diff-1 expr var)
  (cond ((not (pair? expr))
         (if (eq? expr var) 1 0))
        (else
         (let ((op (car expr))
               (args (cdr expr)))
           (cond
             ((eq? op '+)
              (cons '+ (map (lambda (u) (cas-diff-1 u var)) args)))
             ((eq? op '-)
              (if (null? (cdr args))
                  (list '* -1 (cas-diff-1 (car args) var))
                  (cons '+ (cons (cas-diff-1 (car args) var)
                                 (map (lambda (u) (list '* -1 (cas-diff-1 u var))) (cdr args))))))
             ((eq? op '*)
              (cas-diff-mul args var))
             ((eq? op '/)
              (if (null? (cdr args))
                  (let ((v (car args)))
                    (list '/ (list '* -1 (cas-diff-1 v var)) (list '^ v 2)))
                  (let ((u (car args))
                        (v (if (null? (cddr args)) (cadr args) (cons '* (cddr args)))))
                    (list '/
                          (list '-
                                (list '* (cas-diff-1 u var) v)
                                (list '* u (cas-diff-1 v var)))
                          (list '^ v 2)))))
             ((or (eq? op '^) (eq? op 'expt))
              (let ((u (car args))
                    (v (cadr args)))
                (let ((u-has (cas-has-var? u var))
                      (v-has (cas-has-var? v var)))
                  (cond
                    ((and (not u-has) (not v-has)) 0)
                    ((not v-has)
                     ;; u^c: c * u^(c-1) * u'
                     (list '* v (list '* (list '^ u (list '- v 1)) (cas-diff-1 u var))))
                    ((not u-has)
                     ;; a^v: a^v * ln(a) * v'
                     (list '* (list '^ u v) (list '* (list 'log u) (cas-diff-1 v var))))
                    (else
                     ;; u^v: u^v * (v' * ln(u) + v * u' / u)
                     (list '* (list '^ u v)
                           (list '+
                                 (list '* (cas-diff-1 v var) (list 'log u))
                                 (list '/ (list '* v (cas-diff-1 u var)) u))))))))
             ((eq? op 'sqrt)
              ;; d/dx sqrt(u) = d/dx u^(1/2)
              (cas-diff-1 (list '^ (car args) 1/2) var))
             ((eq? op 'exp)
              ;; d/dx exp(u) = exp(u) * u'
              (let ((u (car args)))
                (list '* (list 'exp u) (cas-diff-1 u var))))
             ((eq? op 'log)
              ;; d/dx log(u) = u' / u
              (let ((u (car args)))
                (list '/ (cas-diff-1 u var) u)))
             ((eq? op 'sin)
              ;; d/dx sin(u) = cos(u) * u'
              (let ((u (car args)))
                (list '* (list 'cos u) (cas-diff-1 u var))))
             ((eq? op 'cos)
              ;; d/dx cos(u) = -sin(u) * u'
              (let ((u (car args)))
                (list '* -1 (list '* (list 'sin u) (cas-diff-1 u var)))))
             ((eq? op 'tan)
              ;; d/dx tan(u) = (1 + tan(u)^2) * u'
              (let ((u (car args)))
                (list '* (list '+ 1 (list '^ (list 'tan u) 2)) (cas-diff-1 u var))))
             ((eq? op 'asin)
              ;; d/dx asin(u) = u' / sqrt(1 - u^2)
              (let ((u (car args)))
                (list '/ (cas-diff-1 u var) (list 'sqrt (list '- 1 (list '^ u 2))))))
             ((eq? op 'acos)
              ;; d/dx acos(u) = -u' / sqrt(1 - u^2)
              (let ((u (car args)))
                (list '/ (list '* -1 (cas-diff-1 u var)) (list 'sqrt (list '- 1 (list '^ u 2))))))
             ((eq? op 'atan)
              ;; d/dx atan(u) = u' / (1 + u^2)
              (let ((u (car args)))
                (list '/ (cas-diff-1 u var) (list '+ 1 (list '^ u 2)))))
             ((eq? op 'abs)
              ;; d/dx |u| = (u * u') / |u|
              (let ((u (car args)))
                (list '/ (list '* u (cas-diff-1 u var)) (list 'abs u))))
             (else
              (list 'diff expr var)))))))

(define (diff expr var . rest)
  (let ((n (if (null? rest) 1 (car rest))))
    (cond ((< n 0) (error "diff: negative order"))
          ((= n 0) (simplify expr))
          ((= n 1) (simplify (cas-diff-1 expr var)))
          (else (diff (simplify (cas-diff-1 expr var)) var (- n 1))))))

;;; --- 3. Symbolic Integration (integrate) ---

(define (cas-integrate-by-parts u v var)
  ;; Checks for product of x (or x^n) and transcendental functions
  (cond
    ;; x * exp(a*x + b)
    ((and (eq? u var) (pair? v) (eq? (car v) 'exp))
     (let ((lin (cas-match-linear (cadr v) var)))
       (if lin
           (let ((a (car lin)))
             (list '/
                   (list '* (list 'exp (cadr v)) (list '- (list '* a var) 1))
                   (list '^ a 2)))
           #f)))
    ;; x * cos(a*x + b)
    ((and (eq? u var) (pair? v) (eq? (car v) 'cos))
     (let ((lin (cas-match-linear (cadr v) var)))
       (if lin
           (let ((a (car lin)))
             (list '/
                   (list '+ (list 'cos (cadr v)) (list '* (list '* a var) (list 'sin (cadr v))))
                   (list '^ a 2)))
           #f)))
    ;; x * sin(a*x + b)
    ((and (eq? u var) (pair? v) (eq? (car v) 'sin))
     (let ((lin (cas-match-linear (cadr v) var)))
       (if lin
           (let ((a (car lin)))
             (list '/
                   (list '- (list 'sin (cadr v)) (list '* (list '* a var) (list 'cos (cadr v))))
                   (list '^ a 2)))
           #f)))
    ;; x * log(x)
    ((and (eq? u var) (pair? v) (eq? (car v) 'log) (eq? (cadr v) var))
     (list '+
           (list '* 1/2 (list '* (list '^ var 2) (list 'log var)))
           (list '* -1/4 (list '^ var 2))))
    ;; x^n * log(x) for integer n != -1
    ((and (pair? u) (or (eq? (car u) '^) (eq? (car u) 'expt))
          (eq? (cadr u) var) (integer? (caddr u)) (not (= (caddr u) -1))
          (pair? v) (eq? (car v) 'log) (eq? (cadr v) var))
     (let* ((n (caddr u))
            (n1 (+ n 1))
            (n1-sq (* n1 n1)))
       (list '-
             (list '* (list '/ 1 n1) (list '* (list '^ var n1) (list 'log var)))
             (list '* (list '/ 1 n1-sq) (list '^ var n1)))))
    (else #f)))

(define (cas-integrate-prod-2 u v var)
  (let ((bp1 (cas-integrate-by-parts u v var)))
    (if bp1
        bp1
        (let ((bp2 (cas-integrate-by-parts v u var)))
          (if bp2
              bp2
              ;; If unexpanded polynomials, attempt expansion
              (let ((expanded (expand (list '* u v))))
                (if (and (pair? expanded) (eq? (car expanded) '+))
                    (cons '+ (map (lambda (t) (cas-integrate-term t var)) (cdr expanded)))
                    (list 'integrate (list '* u v) var))))))))

(define (cas-integrate-quadratic-frac num den var)
  (let* ((deg-d (poly-degree den var))
         (deg-n (poly-degree num var)))
    (if (and (= deg-d 2) (<= deg-n 1))
        (let* ((cs-d (poly-coeffs den var))
               (cs-n (poly-coeffs num var))
               (ad (car cs-d))
               (bd (cadr cs-d))
               (cd (caddr cs-d))
               (B (if (= deg-n 1) (car cs-n) 0))
               (C (if (= deg-n 1) (cadr cs-n) (car cs-n))))
          (if (and (equal? bd 0) (number? ad) (> ad 0) (number? cd) (> cd 0))
              (let* ((a2 (/ cd ad))
                     (a-root (isqrt a2))
                     (exact-a? (and (= (* a-root a-root) a2) (> a-root 0)))
                     (a-val (if exact-a? a-root (list 'sqrt a2)))
                     (term-log (if (not (equal? B 0))
                                   (list '* (list '/ B (list '* 2 ad))
                                         (list 'log den))
                                   0))
                     (term-atan (if (not (equal? C 0))
                                    (list '* (list '/ C (list '* ad a-val))
                                          (list 'atan (list '/ var a-val)))
                                    0)))
                (cond ((and (not (equal? B 0)) (not (equal? C 0)))
                       (list '+ term-log term-atan))
                      ((not (equal? B 0)) term-log)
                      ((not (equal? C 0)) term-atan)
                      (else 0)))
              #f))
        #f)))

(define (cas-integrate-term term var)
  (cond
    ;; Constant with respect to var
    ((not (cas-has-var? term var))
     (list '* term var))

    ;; var itself
    ((eq? term var)
     (list '* 1/2 (list '^ var 2)))

    ;; Linear expression: a * var + b -> 1/2 * a * var^2 + b * var
    ((cas-match-linear term var)
     (let* ((lin (cas-match-linear term var))
            (a (car lin))
            (b (cdr lin)))
       (cond ((equal? b 0)
              (list '* 1/2 (list '* a (list '^ var 2))))
             (else
              (list '+
                    (list '* 1/2 (list '* a (list '^ var 2)))
                    (list '* b var))))))

    ;; Powers (^ base exp)
    ((and (pair? term) (or (eq? (car term) '^) (eq? (car term) 'expt)))
     (let ((base (cadr term))
           (exp (caddr term)))
       (cond
         ((not (cas-has-var? exp var))
          (cond
            ((equal? exp -1)
             (cas-integrate-term (list '/ 1 base) var))
            (else
             (let ((lin (cas-match-linear base var)))
               (if lin
                   (let ((a (car lin)))
                     (list '/ (list '^ base (list '+ exp 1))
                           (list '* a (list '+ exp 1))))
                   (list 'integrate term var))))))
         ((not (cas-has-var? base var))
          (let ((lin (cas-match-linear exp var)))
            (if lin
                (let ((a (car lin)))
                  (list '/ term (list '* a (list 'log base))))
                (list 'integrate term var))))
         (else
          (list 'integrate term var)))))

    ;; Division (/ num den)
    ((and (pair? term) (eq? (car term) '/))
     (let ((num (cadr term))
           (den (caddr term)))
       (cond
         ((not (cas-has-var? den var))
          (list '/ (cas-integrate-term num var) den))
         ((equal? num 1)
          (let ((lin (cas-match-linear den var)))
            (cond
              (lin
               (list '/ (list 'log den) (car lin)))
              ;; 1 / sqrt(1 - x^2) -> asin(x)
              ((or (equal? den (list 'sqrt (list '+ 1 (list '* -1 (list '^ var 2)))))
                   (equal? den (list 'sqrt (list '- 1 (list '^ var 2)))))
               (list 'asin var))
              (else
               (let ((qf (cas-integrate-quadratic-frac num den var)))
                 (if qf
                     qf
                     ;; Try partial fraction decomposition
                     (let ((ap (apart num den var)))
                       (if (and (pair? ap) (not (and (eq? (car ap) '/) (equal? (caddr ap) den))))
                           (cas-integrate-term ap var)
                           (list 'integrate term var)))))))))
         (else
          (let ((qf (cas-integrate-quadratic-frac num den var)))
            (if qf
                qf
                (let ((ap (apart num den var)))
                  (if (and (pair? ap) (not (and (eq? (car ap) '/) (equal? (caddr ap) den))))
                      (cas-integrate-term ap var)
                      (list 'integrate term var)))))))))

    ;; Exponentials (exp u)
    ((and (pair? term) (eq? (car term) 'exp))
     (let ((lin (cas-match-linear (cadr term) var)))
       (if lin
           (list '/ term (car lin))
           (list 'integrate term var))))

    ;; Cosine (cos u)
    ((and (pair? term) (eq? (car term) 'cos))
     (let ((lin (cas-match-linear (cadr term) var)))
       (if lin
           (list '/ (list 'sin (cadr term)) (car lin))
           (list 'integrate term var))))

    ;; Sine (sin u)
    ((and (pair? term) (eq? (car term) 'sin))
     (let ((lin (cas-match-linear (cadr term) var)))
       (if lin
           (list '/ (list '* -1 (list 'cos (cadr term))) (car lin))
           (list 'integrate term var))))

    ;; Logarithm (log u)
    ((and (pair? term) (eq? (car term) 'log))
     (let ((u (cadr term)))
       (let ((lin (cas-match-linear u var)))
         (if lin
             (let ((a (car lin)))
               (list '/ (list '- (list '* u (list 'log u)) u) a))
             (list 'integrate term var)))))

    ;; Square root (sqrt u) -> u^(1/2)
    ((and (pair? term) (eq? (car term) 'sqrt))
     (cas-integrate-term (list '^ (cadr term) 1/2) var))

    ;; Products (* f1 f2 ...)
    ((and (pair? term) (eq? (car term) '*))
     (let loop ((factors (cdr term)) (c-acc '()) (v-acc '()))
       (if (null? factors)
           (let ((c (if (null? c-acc) 1 (simplify (cons '* (reverse c-acc))))))
             (cond
               ((null? v-acc)
                (list '* c var))
               ((null? (cdr v-acc))
                (list '* c (cas-integrate-term (car v-acc) var)))
               ((null? (cddr v-acc))
                (list '* c (cas-integrate-prod-2 (car v-acc) (cadr v-acc) var)))
               (else
                ;; Multi-factor products: try expansion
                (let ((expanded (expand (cons '* (reverse v-acc)))))
                  (if (and (pair? expanded) (eq? (car expanded) '+))
                      (list '* c (cons '+ (map (lambda (t) (cas-integrate-term t var)) (cdr expanded))))
                      (list 'integrate term var))))))
           (if (cas-has-var? (car factors) var)
               (loop (cdr factors) c-acc (cons (car factors) v-acc))
               (loop (cdr factors) (cons (car factors) c-acc) v-acc)))))

    ;; Sums (+ t1 t2 ...)
    ((and (pair? term) (eq? (car term) '+))
     (cons '+ (map (lambda (t) (cas-integrate-term t var)) (cdr term))))

    (else
     (list 'integrate term var))))

(define (integrate expr var)
  (let ((s (simplify expr)))
    (simplify (cas-integrate-term s var))))

;;; --- 4. Definite Integration (integrate-def) ---

(define (integrate-def expr var a b)
  (let ((F (integrate expr var)))
    (if (cas-has-op? F 'integrate)
        (list 'integrate-def expr var a b)
        (let ((Fb (simplify (cas-subst F var b)))
              (Fa (simplify (cas-subst F var a))))
          (simplify (list '- Fb Fa))))))

;;; --- 5. Taylor & Maclaurin Power Series Expansion (taylor) ---

(define (cas-factorial n)
  (let loop ((k n) (acc 1))
    (if (<= k 1)
        acc
        (loop (- k 1) (* acc k)))))

(define (taylor expr var order . opt-center)
  (let* ((center (if (null? opt-center) 0 (car opt-center)))
         ;; Low-resource safety: clamp order <= 10
         (n (if (> order 10) 10 (if (< order 0) 0 order))))
    (let loop ((k 0) (cur-d expr) (terms '()))
      (if (> k n)
          (if (null? terms)
              0
              (simplify (cons '+ (reverse terms))))
          (let* ((f-val (simplify (cas-subst cur-d var center)))
                 (fact (cas-factorial k))
                 (coeff (simplify (list '/ f-val fact))))
            (if (equal? coeff 0)
                (loop (+ k 1) (diff cur-d var) terms)
                (let* ((pow-term
                        (cond ((= k 0) 1)
                              ((= k 1)
                               (if (equal? center 0)
                                   var
                                   (list '- var center)))
                              (else
                               (if (equal? center 0)
                                   (list '^ var k)
                                   (list '^ (list '- var center) k)))))
                       (term (if (= k 0)
                                 coeff
                                 (simplify (list '* coeff pow-term)))))
                  (loop (+ k 1) (diff cur-d var) (cons term terms)))))))))
