;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 43.3
;;; Polynomial Algebra, Expansion, Inspection & Evaluation
;;; ============================================================================
;;;
;;; Provides:
;;;   (expand expr)               - Full distributive expansion of products and powers
;;;   (poly-degree expr var)      - Degree of polynomial in var
;;;   (poly-coeffs expr var)      - List of coefficients in descending order of powers
;;;   (poly-lead-coeff expr var)  - Leading coefficient
;;;   (poly-eval expr bindings)   - Evaluate polynomial with variable assignments
;;;   (poly-eval-horner coeffs x) - Evaluate polynomial coefficients using Horner's rule
;;;   (poly-div num den var)      - Polynomial division returning (quotient remainder)
;;;   (poly-add p1 p2)            - Add two polynomials
;;;   (poly-mul p1 p2)            - Multiply two polynomials with expansion
;;;

;;; --- 1. Polynomial Expansion (expand) ---

(define (cas-expand-mul-2 p q)
  (cond ((or (equal? p 0) (equal? q 0)) 0)
        ((equal? p 1) q)
        ((equal? q 1) p)
        ;; Both p and q are sums: (+ a1 a2 ...) * (+ b1 b2 ...)
        ((and (pair? p) (eq? (car p) '+) (pair? q) (eq? (car q) '+))
         (let outer ((ts1 (cdr p)) (all-terms '()))
           (if (null? ts1)
               (simplify (cons '+ (reverse all-terms)))
               (let inner ((ts2 (cdr q)) (sub-terms '()))
                 (if (null? ts2)
                     (outer (cdr ts1) (append sub-terms all-terms))
                     (inner (cdr ts2)
                            (cons (expand (list '* (car ts1) (car ts2)))
                                  sub-terms)))))))
        ;; p is a sum (+ a1 a2 ...): distribute q across each term
        ((and (pair? p) (eq? (car p) '+))
         (simplify (cons '+ (map (lambda (a) (expand (list '* a q))) (cdr p)))))
        ;; q is a sum (+ b1 b2 ...): distribute p across each term
        ((and (pair? q) (eq? (car q) '+))
         (simplify (cons '+ (map (lambda (b) (expand (list '* p b))) (cdr q)))))
        ;; Neither is a sum: direct product simplified
        (else
         (simplify (list '* p q)))))

(define (cas-expand-mul args)
  (let loop ((rest args) (acc 1))
    (if (null? rest)
        acc
        (loop (cdr rest) (cas-expand-mul-2 acc (expand (car rest)))))))

(define (cas-expand-pow base exp)
  (cond ((= exp 0) 1)
        ((= exp 1) base)
        ((= exp 2) (cas-expand-mul-2 base base))
        ((= (remainder exp 2) 0)
         (let ((half (cas-expand-pow base (/ exp 2))))
           (cas-expand-mul-2 half half)))
        (else
         (cas-expand-mul-2 base (cas-expand-pow base (- exp 1))))))

(define (expand expr)
  (cond ((not (pair? expr)) expr)
        ((eq? (car expr) '+)
         (simplify (cons '+ (map expand (cdr expr)))))
        ((eq? (car expr) '-)
         (if (null? (cddr expr))
             (cas-expand-mul-2 -1 (expand (cadr expr)))
             (expand (cons '+ (cons (cadr expr)
                                    (map (lambda (x) (list '* -1 x)) (cddr expr)))))))
        ((eq? (car expr) '*)
         (cas-expand-mul (cdr expr)))
        ((eq? (car expr) '^)
         (let ((b (expand (cadr expr)))
               (e (caddr expr)))
           (if (and (integer? e) (> e 0))
               (cas-expand-pow b e)
               (simplify (list '^ b (expand e))))))
        ((eq? (car expr) '/)
         (simplify (list '/ (expand (cadr expr)) (expand (caddr expr)))))
        (else
         (cons (car expr) (map expand (cdr expr))))))

;;; --- 2. Polynomial Inspection ---

(define (poly-has-var? expr var)
  (cond ((eq? expr var) #t)
        ((pair? expr)
         (or (poly-has-var? (car expr) var)
             (poly-has-var? (cdr expr) var)))
        (else #f)))

(define (poly-term-degree term var)
  (cond ((not (poly-has-var? term var)) 0)
        ((eq? term var) 1)
        ((and (pair? term) (eq? (car term) '^) (eq? (cadr term) var))
         (if (integer? (caddr term)) (caddr term) 1))
        ((and (pair? term) (eq? (car term) '*))
         (let loop ((factors (cdr term)) (deg 0))
           (if (null? factors)
               deg
               (loop (cdr factors) (+ deg (poly-term-degree (car factors) var))))))
        (else 0)))

(define (poly-term-coeff term var)
  (cond ((not (poly-has-var? term var)) term)
        ((eq? term var) 1)
        ((and (pair? term) (eq? (car term) '^) (eq? (cadr term) var)) 1)
        ((and (pair? term) (eq? (car term) '*))
         (let loop ((factors (cdr term)) (c-factors '()))
           (if (null? factors)
               (cond ((null? c-factors) 1)
                     ((null? (cdr c-factors)) (car c-factors))
                     (else (simplify (cons '* (reverse c-factors)))))
               (let ((f (car factors)))
                 (if (poly-has-var? f var)
                     (loop (cdr factors) c-factors)
                     (loop (cdr factors) (cons f c-factors)))))))
        (else term)))

(define (poly-degree expr var)
  (let ((e (simplify (expand expr))))
    (cond ((not (poly-has-var? e var)) 0)
          ((eq? e var) 1)
          ((and (pair? e) (eq? (car e) '^))
           (if (eq? (cadr e) var) (caddr e) 0))
          ((and (pair? e) (eq? (car e) '*))
           (poly-term-degree e var))
          ((and (pair? e) (eq? (car e) '+))
           (let loop ((terms (cdr e)) (max-deg 0))
             (if (null? terms)
                 max-deg
                 (let ((d (poly-term-degree (car terms) var)))
                   (loop (cdr terms) (if (> d max-deg) d max-deg))))))
          (else 0))))

(define (poly-coeffs expr var)
  (let* ((e (simplify (expand expr)))
         (deg (poly-degree e var)))
    (if (= deg 0)
        (list e)
        (let ((terms (if (and (pair? e) (eq? (car e) '+))
                         (cdr e)
                         (list e))))
          (let loop ((k deg) (acc '()))
            (if (< k 0)
                (reverse acc)
                (let ((k-coeffs (let find ((ts terms) (matches '()))
                                  (if (null? ts)
                                      matches
                                      (let ((t (car ts)))
                                        (if (= (poly-term-degree t var) k)
                                            (find (cdr ts) (cons (poly-term-coeff t var) matches))
                                            (find (cdr ts) matches)))))))
                  (let ((coeff (cond ((null? k-coeffs) 0)
                                     ((null? (cdr k-coeffs)) (car k-coeffs))
                                     (else (simplify (cons '+ k-coeffs))))))
                    (loop (- k 1) (cons coeff acc))))))))))

(define (poly-lead-coeff expr var)
  (car (poly-coeffs expr var)))

;;; --- 3. Polynomial Evaluation & Horner's Rule ---

(define (poly-eval-horner coeffs x-val)
  (if (null? coeffs)
      0
      (let loop ((cs (cdr coeffs)) (acc (car coeffs)))
        (if (null? cs)
            (simplify acc)
            (loop (cdr cs) (simplify (list '+ (list '* acc x-val) (car cs))))))))

(define (poly-subst expr bindings)
  (cond ((symbol? expr)
         (let ((pair (assoc expr bindings)))
           (if pair (cdr pair) expr)))
        ((pair? expr)
         (cons (poly-subst (car expr) bindings)
               (poly-subst (cdr expr) bindings)))
        (else expr)))

(define (poly-eval expr bindings)
  (simplify (expand (poly-subst expr bindings))))

;;; --- 4. Polynomial Division (Long Division) ---

(define (poly-div num den var)
  (let ((p1 (simplify (expand num)))
        (p2 (simplify (expand den))))
    (let ((d2 (poly-degree p2 var))
          (lc2 (poly-lead-coeff p2 var)))
      (if (and (= d2 0) (equal? lc2 0))
          (list 'division-by-zero 'division-by-zero)
          (let loop ((r p1) (q 0))
            (let ((dr (poly-degree r var)))
              (if (or (< dr d2) (and (= dr 0) (equal? r 0)))
                  (list (simplify q) (simplify r))
                  (let* ((lcr (poly-lead-coeff r var))
                         (term-c (simplify (list '/ lcr lc2)))
                         (term-deg (- dr d2))
                         (term (cond ((= term-deg 0) term-c)
                                     ((= term-deg 1) (simplify (list '* term-c var)))
                                     (else (simplify (list '* term-c (list '^ var term-deg))))))
                         (new-q (simplify (list '+ q term)))
                         (sub (expand (list '* term p2)))
                         (new-r (simplify (expand (list '- r sub)))))
                    (loop new-r new-q)))))))))

;;; --- 5. High-Level Arithmetic Aliases ---

(define (poly-add p1 p2)
  (simplify (list '+ p1 p2)))

(define (poly-mul p1 p2)
  (expand (list '* p1 p2)))
