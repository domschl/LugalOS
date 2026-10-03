;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 43.5
;;; Exact Equation & Linear System Solving
;;; ============================================================================
;;;
;;; Provides:
;;;   (solve eq var)               - Solves univariate equations (linear, quadratic, etc.)
;;;   (solve-linear eq var)        - Solves degree 1 linear equation: (x . sol)
;;;   (solve-quadratic eq var)     - Solves degree 2 quadratic equation: ((x . s1) (x . s2))
;;;   (solve-system eq-list vars)  - Solves linear system via exact Gaussian elimination
;;;

;;; --- 1. Equation Normalization & Helpers ---

(define (cas-subst expr var val)
  (cond ((equal? expr var) val)
        ((pair? expr)
         (cons (car expr) (map (lambda (x) (cas-subst x var val)) (cdr expr))))
        (else expr)))

(define (cas-eval-zeros expr vars)
  (let loop ((cur expr) (vs vars))
    (if (null? vs)
        (simplify cur)
        (loop (cas-subst cur (car vs) 0) (cdr vs)))))

(define (cas-normalize-eq eq)
  (cond ((and (pair? eq) (eq? (car eq) '=))
         (simplify (list '- (cadr eq) (caddr eq))))
        (else
         (simplify eq))))

;;; --- 2. Linear Equation Solver ---

(define (solve-linear eq var)
  (let* ((E (cas-normalize-eq eq))
         (a (simplify (diff E var)))
         (b (simplify (cas-subst E var 0))))
    (cond ((equal? a 0)
           (if (equal? b 0) 'all 'none))
          (else
           (let ((sol (simplify (list '/ (list '* -1 b) a))))
             (cons var sol))))))

;;; --- 3. Quadratic Equation Solver ---

(define (solve-quadratic eq var)
  (let* ((E (simplify (expand (cas-normalize-eq eq))))
         (coeffs (poly-coeffs E var)))
    (if (not (= (length coeffs) 3))
        (error "solve-quadratic: expected degree 2 equation")
        (let ((a (car coeffs))
              (b (cadr coeffs))
              (c (caddr coeffs)))
          (let* ((four-ac (simplify (list '* 4 (list '* a c))))
                 (b-sq (simplify (list '^ b 2)))
                 (delta (simplify (list '- b-sq four-ac))))
            (cond
              ;; Repeated root: delta = 0
              ((equal? delta 0)
               (let ((sol (simplify (list '/ (list '* -1 b) (list '* 2 a)))))
                 (list (cons var sol))))
              ;; Negative discriminant: express with i
              ((and (number? delta) (< delta 0))
               (let* ((delta-pos (- delta))
                      (sqrt-d (simplify (list '* 'i (list 'sqrt delta-pos))))
                      (two-a (list '* 2 a))
                      (sol1 (simplify (list '/ (list '- (list '* -1 b) sqrt-d) two-a)))
                      (sol2 (simplify (list '/ (list '+ (list '* -1 b) sqrt-d) two-a))))
                 (list (cons var sol1) (cons var sol2))))
              ;; Positive or symbolic discriminant
              (else
               (let* ((sqrt-d (simplify (list 'sqrt delta)))
                      (two-a (list '* 2 a))
                      (sol1 (simplify (list '/ (list '- (list '* -1 b) sqrt-d) two-a)))
                      (sol2 (simplify (list '/ (list '+ (list '* -1 b) sqrt-d) two-a))))
                 (if (and (number? sol1) (number? sol2) (> sol1 sol2))
                     (list (cons var sol2) (cons var sol1))
                     (list (cons var sol1) (cons var sol2)))))))))))

;;; --- 4. General Univariate Solver Dispatch ---

(define (solve eq var)
  (let* ((E (simplify (expand (cas-normalize-eq eq))))
         (deg (poly-degree E var)))
    (cond ((= deg 1)
           (solve-linear E var))
          ((= deg 2)
           (solve-quadratic E var))
          ((= deg 0)
           (if (equal? E 0) 'all 'none))
          (else
           ;; Check for simple monomial power: a * var^n + c = 0
           (let ((coeffs (poly-coeffs E var)))
             (let ((lead (car coeffs))
                   (const-term (car (reverse coeffs)))
                   (mid-zeros?
                    (let loop ((rest (cdr (reverse (cdr coeffs)))))
                      (cond ((null? rest) #t)
                            ((not (equal? (car rest) 0)) #f)
                            (else (loop (cdr rest)))))))
               (if mid-zeros?
                   (let ((sol (simplify (list '^ (list '/ (list '* -1 const-term) lead) (list '/ 1 deg)))))
                     (list (cons var sol)))
                   (list 'solve eq var))))))))

;;; --- 5. Linear Systems & Exact Gaussian Elimination ---

(define (matrix-row M i)
  (list-ref M i))

(define (matrix-elem M i j)
  (list-ref (list-ref M i) j))

(define (matrix-set-row M i new-row)
  (let loop ((rest M) (idx 0) (acc '()))
    (if (null? rest)
        (reverse acc)
        (if (= idx i)
            (loop (cdr rest) (+ idx 1) (cons new-row acc))
            (loop (cdr rest) (+ idx 1) (cons (car rest) acc))))))

(define (matrix-swap-rows M i k)
  (if (= i k)
      M
      (let ((row-i (matrix-row M i))
            (row-k (matrix-row M k)))
        (matrix-set-row (matrix-set-row M i row-k) k row-i))))

(define (row-scale row scalar)
  (map (lambda (x) (simplify (list '* scalar x))) row))

(define (row-sub row1 row2 factor)
  (let loop ((r1 row1) (r2 row2) (acc '()))
    (if (or (null? r1) (null? r2))
        (reverse acc)
        (loop (cdr r1) (cdr r2)
              (cons (simplify (list '- (car r1) (list '* factor (car r2))))
                    acc)))))

;; Finds pivot row k >= j where matrix[k][j] != 0
(define (matrix-find-pivot M j num-rows)
  (let loop ((k j))
    (if (>= k num-rows)
        #f
        (if (not (equal? (matrix-elem M k j) 0))
            k
            (loop (+ k 1))))))

;; Gauss-Jordan elimination on augmented matrix M (n rows, n+1 columns)
(define (gauss-jordan M n)
  (let col-loop ((j 0) (cur-M M))
    (if (>= j n)
        cur-M
        (let ((pivot-k (matrix-find-pivot cur-M j n)))
          (if (not pivot-k)
              #f ; Singular matrix (no unique solution)
              (let* ((swapped-M (matrix-swap-rows cur-M j pivot-k))
                     (pivot-val (matrix-elem swapped-M j j))
                     (inv-p (simplify (list '/ 1 pivot-val)))
                     (norm-row (row-scale (matrix-row swapped-M j) inv-p))
                     (M-norm (matrix-set-row swapped-M j norm-row)))
                (let elim-loop ((i 0) (elim-M M-norm))
                  (if (>= i n)
                      (col-loop (+ j 1) elim-M)
                      (if (= i j)
                          (elim-loop (+ i 1) elim-M)
                          (let ((factor (matrix-elem elim-M i j)))
                            (if (equal? factor 0)
                                (elim-loop (+ i 1) elim-M)
                                (let* ((cur-row (matrix-row elim-M i))
                                       (new-row (row-sub cur-row norm-row factor)))
                                  (elim-loop (+ i 1) (matrix-set-row elim-M i new-row))))))))))))))

(define (solve-system eq-list var-list)
  (let ((n (length var-list)))
    (if (not (= (length eq-list) n))
        (error "solve-system: number of equations must equal number of variables")
        (let ((M (map (lambda (eq)
                        (let* ((E (cas-normalize-eq eq))
                               (coeffs (map (lambda (v) (simplify (diff E v))) var-list))
                               (c (simplify (cas-eval-zeros E var-list)))
                               (b (simplify (list '* -1 c))))
                          (append coeffs (list b))))
                      eq-list)))
          (let ((solved-M (gauss-jordan M n)))
            (if (not solved-M)
                'singular
                (let extract ((j 0) (vs var-list) (sol-acc '()))
                  (if (null? vs)
                      (reverse sol-acc)
                      (let* ((row (matrix-row solved-M j))
                             (val (car (reverse row))))
                        (extract (+ j 1) (cdr vs) (cons (cons (car vs) val) sol-acc)))))))))))
