;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 43.2
;;; Symbolic Expression Canonicalization & Pattern Simplifier
;;; ============================================================================
;;;
;;; Provides:
;;;   (simplify expr)             - Main entry point: simplifies an expression to canonical form
;;;   (cas-expr<? a b)            - Canonical expression total ordering predicate
;;;   (cas-sort lst lt?)          - Functional merge sort on lists
;;;   (cas-flatten-op op args)    - Associative operator flattening
;;;   (simplify-add args)         - Simplifies additive expressions (+ ...)
;;;   (simplify-mul args)         - Simplifies multiplicative expressions (* ...)
;;;   (simplify-pow base exp)     - Simplifies exponentiation (^ base exp)
;;;   (simplify-sqrt x)           - Exact radical extraction & simplification
;;;

;;; --- 1. Total Ordering & Sorting ---

(define (cas-rank x)
  (cond ((number? x) 1)
        ((symbol? x) 2)
        ((pair? x) 3)
        (else 4)))

(define (cas-list<? la lb)
  (cond ((null? la) (not (null? lb)))
        ((null? lb) #f)
        ((cas-expr<? (car la) (car lb)) #t)
        ((cas-expr<? (car lb) (car la)) #f)
        (else (cas-list<? (cdr la) (cdr lb)))))

(define (cas-expr<? a b)
  (let ((ra (cas-rank a))
        (rb (cas-rank b)))
    (cond ((< ra rb) #t)
          ((> ra rb) #f)
          ((= ra 1) (< a b))
          ((= ra 2) (symbol<? a b))
          ((= ra 3)
           (let ((op-a (car a))
                 (op-b (car b)))
             (cond ((cas-expr<? op-a op-b) #t)
                   ((cas-expr<? op-b op-a) #f)
                   (else
                    (let ((la (length a))
                          (lb (length b)))
                      (cond ((< la lb) #t)
                            ((> la lb) #f)
                            (else (cas-list<? (cdr a) (cdr b)))))))))
          (else #f))))

(define (cas-merge a b lt?)
  (cond ((null? a) b)
        ((null? b) a)
        ((lt? (car b) (car a))
         (cons (car b) (cas-merge a (cdr b) lt?)))
        (else
         (cons (car a) (cas-merge (cdr a) b lt?)))))

(define (cas-split-half lst)
  (if (or (null? lst) (null? (cdr lst)))
      (cons lst '())
      (let split-helper ((fast (cddr lst))
                         (slow (cdr lst))
                         (acc (list (car lst))))
        (if (or (null? fast) (null? (cdr fast)))
            (cons (reverse acc) slow)
            (split-helper (cddr fast) (cdr slow) (cons (car slow) acc))))))

(define (cas-sort lst lt?)
  (if (or (null? lst) (null? (cdr lst)))
      lst
      (let* ((halves (cas-split-half lst))
             (left (car halves))
             (right (cdr halves)))
        (cas-merge (cas-sort left lt?)
                   (cas-sort right lt?)
                   lt?))))

;;; --- 2. Associative Flattening ---

(define (cas-flatten-op op args)
  (let flatten-helper ((items args))
    (cond ((null? items) '())
          ((and (pair? (car items)) (eq? (caar items) op))
           (append (flatten-helper (cdar items))
                   (flatten-helper (cdr items))))
          (else
           (cons (car items) (flatten-helper (cdr items)))))))

;;; --- 3. Exact Integer Square Root Factorization ---

(define cas-small-prime-squares
  '(4 9 25 49 121 169 289 361 529 841 961 1369 1681 1849 2209 2809 3481 3721 4489 5041 5329 6241 6889 7921 9409))

(define (cas-sqrt-factor n)
  (if (<= n 0)
      (list 0 0)
      (let ((s (isqrt n)))
        (if (= (* s s) n)
            (list s 1)
            (let extract ((sq-list cas-small-prime-squares)
                          (curr-d 1)
                          (curr-rem n))
              (cond ((null? sq-list)
                     (let ((rem-s (isqrt curr-rem)))
                       (if (= (* rem-s rem-s) curr-rem)
                           (list (* curr-d rem-s) 1)
                           (list curr-d curr-rem))))
                    (else
                     (let ((sq (car sq-list)))
                       (if (> sq curr-rem)
                           (extract '() curr-d curr-rem)
                           (if (= (remainder curr-rem sq) 0)
                               (let ((p (isqrt sq)))
                                 (extract sq-list (* curr-d p) (quotient curr-rem sq)))
                               (extract (cdr sq-list) curr-d curr-rem)))))))))))

(define (simplify-sqrt x)
  (cond ((equal? x 0) 0)
        ((equal? x 1) 1)
        ((and (integer? x) (> x 0))
         (let* ((fact (cas-sqrt-factor x))
                (d (car fact))
                (rem (cadr fact)))
           (cond ((= rem 1) d)
                 ((= d 1) (list 'sqrt rem))
                 (else (list '* d (list 'sqrt rem))))))
        ((and (number? x) (> x 0))
         (let* ((num (numerator x))
                (den (denominator x))
                (prod (* num den))
                (fact (cas-sqrt-factor prod))
                (d (car fact))
                (rem (cadr fact))
                (coeff (/ d den)))
           (cond ((= rem 1) coeff)
                 ((= coeff 1) (list 'sqrt rem))
                 (else (list '* coeff (list 'sqrt rem))))))
        ((and (pair? x) (eq? (car x) '^))
         (let ((b (cadr x))
               (p (caddr x)))
           (cond ((equal? p 2) b)
                 ((and (integer? p) (= (remainder p 2) 0))
                  (list '^ b (quotient p 2)))
                 (else (list '^ b (simplify (list '* p 1/2)))))))
        (else
         (list 'sqrt x))))

;;; --- 4. Exponentiation ---

(define (simplify-pow base exp)
  (cond ((equal? exp 0) 1)
        ((equal? exp 1) base)
        ((equal? base 1) 1)
        ((and (number? base) (= base 0) (number? exp) (> exp 0)) 0)
        ((and (number? base) (number? exp))
         (if (integer? exp)
             (expt base exp)
             (if (equal? exp 1/2)
                 (simplify-sqrt base)
                 (list '^ base exp))))
        ((and (pair? base) (eq? (car base) '^))
         (let ((b (cadr base))
               (p1 (caddr base)))
           (simplify (list '^ b (simplify (list '* p1 exp))))))
        ((and (pair? base) (eq? (car base) '*) (integer? exp))
         (simplify (cons '* (map (lambda (f) (list '^ f exp)) (cdr base)))))
        ((equal? exp 1/2)
         (simplify-sqrt base))
        (else
         (list '^ base exp))))

;;; --- 5. Multiplication ---

(define (cas-mul-decompose-factor f)
  (if (and (pair? f) (eq? (car f) '^))
      (cons (cadr f) (caddr f))
      (cons f 1)))

(define (simplify-mul raw-args)
  (let* ((args (cas-flatten-op '* raw-args))
         (num-parts '())
         (var-parts '()))
    (for-each (lambda (x)
                (if (number? x)
                    (set! num-parts (cons x num-parts))
                    (set! var-parts (cons x var-parts))))
              args)
    (let ((c-prod (apply * (cons 1 num-parts))))
      (cond ((= c-prod 0) 0)
            ((null? var-parts) c-prod)
            (else
             (let* ((decomposed (map cas-mul-decompose-factor var-parts))
                    (sorted (cas-sort decomposed (lambda (p1 p2) (cas-expr<? (car p1) (car p2)))))
                    (collected-rev
                     (let collect-factors ((sorted-pairs (cdr sorted))
                                           (current-base (caar sorted))
                                           (current-pow (cdar sorted))
                                           (acc '()))
                       (cond ((null? sorted-pairs)
                              (let ((simp-p (simplify current-pow)))
                                (cond ((equal? simp-p 0) acc)
                                      ((equal? simp-p 1) (cons current-base acc))
                                      (else (cons (list '^ current-base simp-p) acc)))))
                             (else
                              (let* ((pair (car sorted-pairs))
                                     (b (car pair))
                                     (p (cdr pair)))
                                (if (equal? b current-base)
                                    (collect-factors (cdr sorted-pairs)
                                                     current-base
                                                     (list '+ current-pow p)
                                                     acc)
                                    (let ((simp-p (simplify current-pow)))
                                      (let ((new-acc
                                             (cond ((equal? simp-p 0) acc)
                                                   ((equal? simp-p 1) (cons current-base acc))
                                                   (else (cons (list '^ current-base simp-p) acc)))))
                                        (collect-factors (cdr sorted-pairs) b p new-acc)))))))))
                    (factors (reverse collected-rev)))
               (cond ((null? factors) c-prod)
                     ((= c-prod 1)
                      (if (null? (cdr factors))
                          (car factors)
                          (cons '* factors)))
                     (else
                      (cons '* (cons c-prod factors))))))))))

;;; --- 6. Addition ---

(define (cas-add-decompose-term t)
  (if (and (pair? t) (eq? (car t) '*))
      (let ((rest (cdr t)))
        (if (and (pair? rest) (number? (car rest)))
            (let ((c (car rest))
                  (factors (cdr rest)))
              (cond ((null? factors) (cons c 1))
                    ((null? (cdr factors)) (cons c (car factors)))
                    (else (cons c (cons '* factors)))))
            (cons 1 t)))
      (cons 1 t)))

(define (simplify-add raw-args)
  (let* ((args (cas-flatten-op '+ raw-args))
         (num-parts '())
         (var-parts '()))
    (for-each (lambda (x)
                (if (number? x)
                    (set! num-parts (cons x num-parts))
                    (set! var-parts (cons x var-parts))))
              args)
    (let ((c-sum (apply + (cons 0 num-parts))))
      (if (null? var-parts)
          c-sum
          (let* ((decomposed (map cas-add-decompose-term var-parts))
                 (sorted (cas-sort decomposed (lambda (p1 p2) (cas-expr<? (cdr p1) (cdr p2)))))
                 (collected-rev
                  (let collect-terms ((sorted-pairs (cdr sorted))
                                      (current-base (cdar sorted))
                                      (current-c (caar sorted))
                                      (acc '()))
                    (cond ((null? sorted-pairs)
                           (let ((c (simplify current-c)))
                             (cond ((equal? c 0) acc)
                                   ((equal? c 1) (cons current-base acc))
                                   ((and (pair? current-base) (eq? (car current-base) '*))
                                    (cons (cons '* (cons c (cdr current-base))) acc))
                                   (else (cons (list '* c current-base) acc)))))
                          (else
                           (let* ((pair (car sorted-pairs))
                                  (c (car pair))
                                  (b (cdr pair)))
                             (if (equal? b current-base)
                                 (collect-terms (cdr sorted-pairs)
                                                current-base
                                                (list '+ current-c c)
                                                acc)
                                 (let ((sc (simplify current-c)))
                                   (let ((new-acc
                                          (cond ((equal? sc 0) acc)
                                                ((equal? sc 1) (cons current-base acc))
                                                ((and (pair? current-base) (eq? (car current-base) '*))
                                                 (cons (cons '* (cons sc (cdr current-base))) acc))
                                                (else (cons (list '* sc current-base) acc)))))
                                     (collect-terms (cdr sorted-pairs) b c new-acc)))))))))
                 (terms (reverse collected-rev)))
            (cond ((null? terms) c-sum)
                  ((= c-sum 0)
                   (if (null? (cdr terms))
                       (car terms)
                       (cons '+ terms)))
                  (else
                   (cons '+ (cons c-sum terms)))))))))

;;; --- 7. Subtraction & Division ---

(define (simplify-sub args)
  (cond ((null? args) 0)
        ((null? (cdr args))
         (simplify (list '* -1 (car args))))
        (else
         (simplify (cons '+ (cons (car args)
                                  (map (lambda (x) (list '* -1 x)) (cdr args))))))))

(define (simplify-div args)
  (cond ((null? args) 1)
        ((null? (cdr args))
         (simplify (list '^ (car args) -1)))
        (else
         (simplify (cons '* (cons (car args)
                                  (map (lambda (x) (list '^ x -1)) (cdr args))))))))

;;; --- 8. Transcendental & Special Functions ---

(define (simplify-exp x)
  (cond ((equal? x 0) 1)
        (else (list 'exp x))))

(define (simplify-log x)
  (cond ((equal? x 1) 0)
        ((and (pair? x) (eq? (car x) 'exp))
         (cadr x))
        (else (list 'log x))))

(define (simplify-sin x)
  (cond ((equal? x 0) 0)
        (else (list 'sin x))))

(define (simplify-cos x)
  (cond ((equal? x 0) 1)
        (else (list 'cos x))))

(define (simplify-tan x)
  (cond ((equal? x 0) 0)
        (else (list 'tan x))))

(define (simplify-abs x)
  (cond ((number? x) (abs x))
        ((and (pair? x) (eq? (car x) '*))
         (let ((c (cas-add-decompose-term x)))
           (if (< (car c) 0)
               (list 'abs (simplify (list '* (- (car c)) (cdr c))))
               (list 'abs x))))
        (else (list 'abs x))))

;;; --- 9. Main Simplifier Dispatch & Fixed-Point Loop ---

(define (simplify-step expr)
  (cond ((not (pair? expr)) expr)
        (else
         (let ((op (car expr))
               (args (map simplify (cdr expr))))
           (cond ((eq? op '+) (simplify-add args))
                 ((eq? op '*) (simplify-mul args))
                 ((eq? op '-) (simplify-sub args))
                 ((eq? op '/) (simplify-div args))
                 ((or (eq? op '^) (eq? op 'expt))
                  (if (= (length args) 2)
                      (simplify-pow (car args) (cadr args))
                      (cons op args)))
                 ((eq? op 'sqrt)
                  (if (= (length args) 1)
                      (simplify-sqrt (car args))
                      (cons op args)))
                 ((eq? op 'exp)
                  (if (= (length args) 1)
                      (simplify-exp (car args))
                      (cons op args)))
                 ((eq? op 'log)
                  (if (= (length args) 1)
                      (simplify-log (car args))
                      (cons op args)))
                 ((eq? op 'sin)
                  (if (= (length args) 1)
                      (simplify-sin (car args))
                      (cons op args)))
                 ((eq? op 'cos)
                  (if (= (length args) 1)
                      (simplify-cos (car args))
                      (cons op args)))
                 ((eq? op 'tan)
                  (if (= (length args) 1)
                      (simplify-tan (car args))
                      (cons op args)))
                 ((eq? op 'abs)
                  (if (= (length args) 1)
                      (simplify-abs (car args))
                      (cons op args)))
                 (else
                  (cons op args)))))))

(define (simplify expr)
  (let loop ((cur expr) (count 10))
    (if (<= count 0)
        cur
        (let ((next (simplify-step cur)))
          (if (equal? cur next)
              cur
              (loop next (- count 1)))))))
