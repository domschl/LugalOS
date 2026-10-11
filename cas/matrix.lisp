;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 48.3c
;;; Exact Symbolic & Rational Linear Algebra (cas/matrix.lisp)
;;; ============================================================================
;;;
;;; Provides:
;;;   (matrix-rows A)              - Number of rows
;;;   (matrix-cols A)              - Number of columns
;;;   (matrix-ref A r c)           - Element at row r, column c (0-indexed)
;;;   (matrix-zero rows cols)      - Zero matrix
;;;   (matrix-identity n)          - Identity matrix of size n
;;;   (matrix-add A B)             - Matrix addition A + B
;;;   (matrix-sub A B)             - Matrix subtraction A - B
;;;   (matrix-scale A s)           - Scalar multiplication s * A
;;;   (matrix-transpose A)         - Transpose A^T
;;;   (vector-dot u v)             - Inner product u . v (lists or vectors)
;;;   (vector-cross u v)           - 3D cross product u x v
;;;   (matrix-mul A B)             - Matrix multiplication A * B
;;;   (matrix-det A)               - Exact determinant det(A)
;;;   (matrix-inv A)               - Exact matrix inverse A^-1
;;;   (matrix-solve A B)           - Solve linear system A * X = B
;;;

;;; --- 1. Dimension & Basic Utilities ---

(define (matrix-rows A)
  (length A))

(define (matrix-cols A)
  (if (null? A) 0 (length (car A))))

(define (matrix-ref A r c)
  (list-ref (list-ref A r) c))

(define (matrix-zero rows cols)
  (let loop-r ((r rows) (res '()))
    (if (<= r 0)
        res
        (let loop-c ((c cols) (row '()))
          (if (<= c 0)
              (loop-r (- r 1) (cons row res))
              (loop-c (- c 1) (cons 0 row)))))))

(define (matrix-identity n)
  (let loop-r ((i 0) (res '()))
    (if (>= i n)
        (reverse res)
        (let loop-c ((j 0) (row '()))
          (if (>= j n)
              (loop-r (+ i 1) (cons (reverse row) res))
              (loop-c (+ j 1) (cons (if (= i j) 1 0) row)))))))

;;; --- 2. Element-wise Operations & Transposition ---

(define (matrix-add A B)
  (map (lambda (rowA rowB)
         (map (lambda (a b) (simplify (list '+ a b))) rowA rowB))
       A B))

(define (matrix-sub A B)
  (map (lambda (rowA rowB)
         (map (lambda (a b) (simplify (list '- a b))) rowA rowB))
       A B))

(define (matrix-scale A s)
  (map (lambda (row)
         (map (lambda (x) (simplify (list '* s x))) row))
       A))

(define (matrix-transpose A)
  (if (or (null? A) (null? (car A)))
      '()
      (cons (map car A) (matrix-transpose (map cdr A)))))

;;; --- 3. Vector Operations ---

(define (matrix-vec->list v)
  (if (vector? v) (vector->list v) v))

(define (vector-dot u v)
  (let ((ul (matrix-vec->list u))
        (vl (matrix-vec->list v)))
    (simplify (cons '+ (map (lambda (a b) (list '* a b)) ul vl)))))

(define (vector-cross u v)
  (let ((ul (matrix-vec->list u))
        (vl (matrix-vec->list v)))
    (let ((u1 (car ul))  (u2 (cadr ul))  (u3 (caddr ul))
          (v1 (car vl))  (v2 (cadr vl))  (v3 (caddr vl)))
      (let ((w1 (simplify (list '- (list '* u2 v3) (list '* u3 v2))))
            (w2 (simplify (list '- (list '* u3 v1) (list '* u1 v3))))
            (w3 (simplify (list '- (list '* u1 v2) (list '* u2 v1)))))
        (if (vector? u)
            (vector w1 w2 w3)
            (list w1 w2 w3))))))

;;; --- 4. Matrix Multiplication ---

(define (matrix-mul A B)
  (let ((BT (matrix-transpose B)))
    (map (lambda (rowA)
           (map (lambda (colB) (vector-dot rowA colB)) BT))
         A)))

;;; --- 5. Row Operations & Helpers ---

(define (matrix-swap-rows rows i j)
  (if (= i j)
      rows
      (let ((row-i (list-ref rows i))
            (row-j (list-ref rows j)))
        (let loop ((k 0) (rest rows) (acc '()))
          (if (null? rest)
              (reverse acc)
              (let ((cur (cond ((= k i) row-j)
                               ((= k j) row-i)
                               (else (car rest)))))
                (loop (+ k 1) (cdr rest) (cons cur acc))))))))

(define (matrix-replace-row rows i new-row)
  (let loop ((k 0) (rest rows) (acc '()))
    (if (null? rest)
        (reverse acc)
        (let ((cur (if (= k i) new-row (car rest))))
          (loop (+ k 1) (cdr rest) (cons cur acc))))))

;;; --- 6. Determinant (matrix-det) ---

(define (matrix-det A)
  (let ((n (matrix-rows A)))
    (cond
      ((= n 0) 1)
      ((= n 1) (car (car A)))
      ((= n 2)
       (let ((a (list-ref (list-ref A 0) 0))
             (b (list-ref (list-ref A 0) 1))
             (c (list-ref (list-ref A 1) 0))
             (d (list-ref (list-ref A 1) 1)))
         (simplify (list '- (list '* a d) (list '* b c)))))
      (else
       ;; Gaussian elimination with row swaps
       (let elim ((k 0) (M A) (sign 1))
         (if (>= k n)
             ;; Product of diagonal elements * sign
             (let prod-diag ((i 0) (acc sign))
               (if (>= i n)
                   acc
                   (prod-diag (+ i 1)
                              (simplify (list '* acc (list-ref (list-ref M i) i))))))
             ;; Find pivot row >= k
             (let ((p (let find-p ((i k))
                        (if (>= i n)
                            #f
                            (if (not (equal? (simplify (list-ref (list-ref M i) k)) 0))
                                i
                                (find-p (+ i 1)))))))
               (if (not p)
                   0
                   (let* ((swapped (if (= p k) M (matrix-swap-rows M k p)))
                          (new-sign (if (= p k) sign (* sign -1)))
                          (pivot (list-ref (list-ref swapped k) k))
                          (row-k (list-ref swapped k)))
                     ;; Eliminate below pivot
                     (let elim-rows ((i (+ k 1)) (cur-M swapped))
                       (if (>= i n)
                           (elim (+ k 1) cur-M new-sign)
                           (let ((val (list-ref (list-ref cur-M i) k)))
                             (if (equal? (simplify val) 0)
                                 (elim-rows (+ i 1) cur-M)
                                 (let* ((factor (simplify (list '/ val pivot)))
                                        (row-i (list-ref cur-M i))
                                        (new-row-i
                                         (map (lambda (xi xk)
                                                (simplify (list '- xi (list '* factor xk))))
                                              row-i row-k)))
                                   (elim-rows (+ i 1)
                                              (matrix-replace-row cur-M i new-row-i))))))))))))))))

;;; --- 7. Matrix Inversion (matrix-inv) & Linear Solve (matrix-solve) ---

(define (matrix-inv A)
  (let* ((n (matrix-rows A))
         (I (matrix-identity n))
         (aug (map (lambda (rowA rowI) (append rowA rowI)) A I)))
    ;; Gauss-Jordan elimination on [A | I]
    (let elim ((k 0) (M aug))
      (if (>= k n)
          ;; Extract right half (columns n .. 2n-1)
          (map (lambda (row)
                 (let drop-loop ((cnt n) (rest row))
                   (if (<= cnt 0) rest (drop-loop (- cnt 1) (cdr rest)))))
               M)
          ;; Find pivot row >= k
          (let ((p (let find-p ((i k))
                     (if (>= i n)
                         #f
                         (if (not (equal? (simplify (list-ref (list-ref M i) k)) 0))
                             i
                             (find-p (+ i 1)))))))
            (if (not p)
                #f ; Singular matrix
                (let* ((swapped (if (= p k) M (matrix-swap-rows M k p)))
                       (pivot (list-ref (list-ref swapped k) k))
                       ;; Scale row k to make pivot = 1
                       (row-k (map (lambda (x) (simplify (list '/ x pivot)))
                                   (list-ref swapped k)))
                       (scaled-M (matrix-replace-row swapped k row-k)))
                  ;; Eliminate all other rows i != k
                  (let elim-other ((i 0) (cur-M scaled-M))
                    (cond
                      ((>= i n) (elim (+ k 1) cur-M))
                      ((= i k) (elim-other (+ i 1) cur-M))
                      (else
                       (let ((factor (list-ref (list-ref cur-M i) k)))
                         (if (equal? (simplify factor) 0)
                             (elim-other (+ i 1) cur-M)
                             (let* ((row-i (list-ref cur-M i))
                                    (new-row-i
                                     (map (lambda (xi xk)
                                            (simplify (list '- xi (list '* factor xk))))
                                          row-i row-k)))
                               (elim-other (+ i 1)
                                           (matrix-replace-row cur-M i new-row-i)))))))))))))))

(define (matrix-solve A B)
  (let* ((n (matrix-rows A))
         (is-vec (not (pair? (car B))))
         (aug (if is-vec
                  (map (lambda (rowA b-elem) (append rowA (list b-elem))) A B)
                  (map (lambda (rowA rowB) (append rowA rowB)) A B))))
    (let elim ((k 0) (M aug))
      (if (>= k n)
          ;; Extract solution
          (if is-vec
              (map (lambda (row) (simplify (car (reverse row)))) M)
              (map (lambda (row)
                     (let drop-loop ((cnt n) (rest row))
                       (if (<= cnt 0) rest (drop-loop (- cnt 1) (cdr rest)))))
                   M))
          ;; Pivot
          (let ((p (let find-p ((i k))
                     (if (>= i n)
                         #f
                         (if (not (equal? (simplify (list-ref (list-ref M i) k)) 0))
                             i
                             (find-p (+ i 1)))))))
            (if (not p)
                #f ; Singular or dependent system
                (let* ((swapped (if (= p k) M (matrix-swap-rows M k p)))
                       (pivot (list-ref (list-ref swapped k) k))
                       (row-k (map (lambda (x) (simplify (list '/ x pivot)))
                                   (list-ref swapped k)))
                       (scaled-M (matrix-replace-row swapped k row-k)))
                  (let elim-other ((i 0) (cur-M scaled-M))
                    (cond
                      ((>= i n) (elim (+ k 1) cur-M))
                      ((= i k) (elim-other (+ i 1) cur-M))
                      (else
                       (let ((factor (list-ref (list-ref cur-M i) k)))
                         (if (equal? (simplify factor) 0)
                             (elim-other (+ i 1) cur-M)
                             (let* ((row-i (list-ref cur-M i))
                                    (new-row-i
                                     (map (lambda (xi xk)
                                            (simplify (list '- xi (list '* factor xk))))
                                          row-i row-k)))
                               (elim-other (+ i 1)
                                           (matrix-replace-row cur-M i new-row-i)))))))))))))))
