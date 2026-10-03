;;; ============================================================================
;;; LugalOS CAS (Computer Algebra System) - Milestone 43.6
;;; Bidirectional Infix <-> Prefix Conversion & Math Notation Front-End
;;; ============================================================================
;;;
;;; Provides:
;;;   (to-infix ast)       - Prefix AST -> infix string:   "sin(x) + x*cos(x)"
;;;   (from-infix string)  - Infix string -> prefix AST:   "3x^2 + 2x" -> (+ (* 3 (^ x 2)) (* 2 x))
;;;   (calc string)        - Parse, run CAS commands, simplify -> result AST
;;;   (math string)        - Like calc, but prints the result in infix notation
;;;
;;; Input syntax:
;;;   numbers      42   3.14 (exact: 157/50)   .5
;;;   operators    + - * / ^ (also **)   implicit multiplication: 2x  3sin(x)  x(x+1)  (x+1)(x-1)
;;;   precedence   -x^2 = -(x^2),  a^b^c = a^(b^c),  2^-3 works
;;;   functions    sin cos tan exp ln log sqrt abs asin acos atan
;;;   commands     diff(f, x)  diff(f, x, n)  integrate(f, x)  integrate(f, x, a, b)
;;;                expand(f)  simplify(f)  solve(a = b, x)  solve([e1, e2], [x, y])
;;;   equations    a = b           lists   [a, b, c]
;;;   constants    pi  e (kept as symbols)
;;;   names        multi-letter names are split into products ("xy" = x*y) unless
;;;                they are a known function/constant; names containing a digit or
;;;                underscore (x1, a_b) are kept whole.
;;;
;;; Requires simplify.lisp, poly.lisp, calculus.lisp, solve.lisp (see cas.lisp).
;;;

;;; --- 0. Shared helpers ---

(define (cf-join strs sep)
  (if (null? strs)
      ""
      (let loop ((rest (cdr strs)) (acc (car strs)))
        (if (null? rest)
            acc
            (loop (cdr rest) (string-append acc sep (car rest)))))))

(define (cf-ch s i) (substring s i (+ i 1)))

(define (cf-digit? c)
  (and (not (string<? c "0")) (not (string>? c "9"))))

(define (cf-alpha? c)
  (or (and (not (string<? c "a")) (not (string>? c "z")))
      (and (not (string<? c "A")) (not (string>? c "Z")))))

(define (cf-alnum? c)
  (or (cf-alpha? c) (cf-digit? c) (string=? c "_")))

(define (cf-space? c)
  (or (string=? c " ") (string=? c "\t") (string=? c "\n") (string=? c "\r")))

;;; --- 1. Formatter: prefix AST -> infix string ---
;;;
;;; Precedence: 0 equation, 1 sum / negative term, 2 product & quotient,
;;; 4 power, 9 atom & function call. Each formatter returns (string . prec).

(define (cf-neg-num? x) (and (number? x) (< x 0)))

(define (cf-fmt e ctx)
  (let ((r (cf-raw e)))
    (if (< (cdr r) ctx)
        (string-append "(" (car r) ")")
        (car r))))

(define (cf-fmt-list items)
  (map (lambda (x) (cf-fmt x 0)) items))

;; (negative? . positive-term) for use in sums
(define (cf-split-sign t)
  (cond ((cf-neg-num? t) (cons #t (- t)))
        ((and (pair? t) (eq? (car t) '*) (pair? (cdr t)) (cf-neg-num? (cadr t)))
         (let ((c (- (cadr t)))
               (rest (cddr t)))
           (cons #t
                 (cond ((null? rest) c)
                       ((equal? c 1)
                        (if (null? (cdr rest)) (car rest) (cons '* rest)))
                       (else (cons '* (cons c rest)))))))
        (else (cons #f t))))

(define (cf-order-terms terms)
  (let loop ((ts terms) (pos '()) (neg '()))
    (if (null? ts)
        (append (reverse pos) (reverse neg))
        (let ((sp (cf-split-sign (car ts))))
          (if (car sp)
              (loop (cdr ts) pos (cons (car ts) neg))
              (loop (cdr ts) (cons (car ts) pos) neg))))))

(define (cf-raw-sum terms)
  (let loop ((ts (cf-order-terms terms)) (acc "") (first #t))
    (if (null? ts)
        (cons acc 1)
        (let* ((sp (cf-split-sign (car ts)))
               (neg (car sp))
               (body (cf-fmt (cdr sp) (if neg 2 1)))
               (piece (cond (first (if neg (string-append "-" body) body))
                            (neg (string-append " - " body))
                            (else (string-append " + " body)))))
          (loop (cdr ts) (string-append acc piece) #f)))))

(define (cf-raw-product factors)
  (let* ((c (if (and (pair? factors) (number? (car factors))) (car factors) 1))
         (others (if (and (pair? factors) (number? (car factors))) (cdr factors) factors))
         (neg (< c 0))
         (ac (if neg (- c) c))
         (p (numerator ac))
         (q (denominator ac)))
    (let split ((fs others) (nums '()) (dens '()))
      (if (pair? fs)
          (let ((f (car fs)))
            (if (and (pair? f) (eq? (car f) '^) (cf-neg-num? (caddr f)))
                (split (cdr fs) nums
                       (cons (if (equal? (caddr f) -1) (cadr f) (list '^ (cadr f) (- (caddr f)))) dens))
                (split (cdr fs) (cons f nums) dens)))
          (let* ((nums (reverse nums))
                 (dens (let ((d (reverse dens))) (if (equal? q 1) d (cons q d))))
                 (num-strs (let ((ss (map (lambda (x) (cf-fmt x 3)) nums)))
                             (if (equal? p 1)
                                 (if (null? ss) (list "1") ss)
                                 (cons (number->string p) ss))))
                 (num-str (cf-join num-strs "*"))
                 (den-str (cond ((null? dens) #f)
                                ((null? (cdr dens)) (cf-fmt (car dens) 3))
                                (else (string-append "("
                                                     (cf-join (map (lambda (x) (cf-fmt x 3)) dens) "*")
                                                     ")"))))
                 (body (if den-str (string-append num-str "/" den-str) num-str)))
            (cond (neg (cons (string-append "-" body) 1))
                  (else (cons body 2))))))))

(define (cf-fn-name op)
  (if (eq? op 'log) "ln" (symbol->string op)))

(define (cf-raw e)
  (cond
    ((number? e)
     (cons (number->string e) (cond ((< e 0) 1) ((integer? e) 9) (else 2))))
    ((symbol? e) (cons (symbol->string e) 9))
    ((string? e) (cons e 9))
    ((not (pair? e)) (cons (if e "true" "false") 9))
    ;; (x . 2) solution pair
    ((and (symbol? (car e)) (not (pair? (cdr e))) (not (null? (cdr e))))
     (cons (string-append (symbol->string (car e)) " = " (cf-fmt (cdr e) 0)) 0))
    ;; list of things: [a, b, c]
    ((pair? (car e))
     (cons (string-append "[" (cf-join (cf-fmt-list e) ", ") "]") 9))
    ((eq? (car e) 'cas-list)
     (cons (string-append "[" (cf-join (cf-fmt-list (cdr e)) ", ") "]") 9))
    ((eq? (car e) '+)
     (cond ((null? (cdr e)) (cons "0" 9))
           (else (cf-raw-sum (reverse (cdr e))))))
    ((eq? (car e) '*)
     (cond ((null? (cdr e)) (cons "1" 9))
           (else (cf-raw-product (cdr e)))))
    ((eq? (car e) '-)
     (cond ((null? (cdr e)) (cons "0" 9))
           ((null? (cddr e)) (cons (string-append "-" (cf-fmt (cadr e) 2)) 1))
           (else
            (cons (let loop ((rest (cddr e)) (acc (cf-fmt (cadr e) 1)))
                    (if (null? rest)
                        acc
                        (loop (cdr rest) (string-append acc " - " (cf-fmt (car rest) 2)))))
                  1))))
    ((eq? (car e) '/)
     (cons (string-append (cf-fmt (cadr e) 2) "/" (cf-fmt (caddr e) 3)) 2))
    ((or (eq? (car e) '^) (eq? (car e) 'expt))
     (if (cf-neg-num? (caddr e))
         (cf-raw-product (list (list '^ (cadr e) (caddr e))))
         (cons (string-append (cf-fmt (cadr e) 5) "^" (cf-fmt (caddr e) 4)) 4)))
    ((eq? (car e) '=)
     (cons (string-append (cf-fmt (cadr e) 1) " = " (cf-fmt (caddr e) 1)) 0))
    (else
     (cons (string-append (cf-fn-name (car e)) "(" (cf-join (cf-fmt-list (cdr e)) ", ") ")") 9))))

(define (to-infix e) (cf-fmt e 0))

;;; --- 2. Tokenizer ---

(define cf-fn-names '("sin" "cos" "tan" "exp" "ln" "log" "sqrt" "abs" "asin" "acos" "atan"
                      "diff" "integrate" "expand" "simplify" "solve"))
(define cf-known-names (append cf-fn-names '("pi" "e")))

(define cf-err #f)

(define (cf-fail msg)
  (if (not cf-err) (set! cf-err msg))
  #f)

(define (cf-scan s i n pred)
  (let loop ((j i))
    (if (and (< j n) (pred (cf-ch s j))) (loop (+ j 1)) j)))

(define (cf-has-digit-or-us? run)
  (let ((n (string-length run)))
    (let loop ((i 0))
      (cond ((>= i n) #f)
            ((let ((c (cf-ch run i))) (or (cf-digit? c) (string=? c "_"))) #t)
            (else (loop (+ i 1)))))))

;; Length of the longest known name found at position k in run, or 0
(define (cf-match-known run k n)
  (let loop ((names cf-known-names) (best 0))
    (if (null? names)
        best
        (let ((len (string-length (car names))))
          (loop (cdr names)
                (if (and (> len best)
                         (<= (+ k len) n)
                         (string=? (substring run k (+ k len)) (car names)))
                    len
                    best))))))

(define (cf-split-ident run acc)
  (let ((n (string-length run)))
    (let loop ((k 0) (acc acc))
      (if (>= k n)
          acc
          (let ((m (cf-match-known run k n)))
            (if (> m 0)
                (loop (+ k m) (cons (string->symbol (substring run k (+ k m))) acc))
                (loop (+ k 1) (cons (string->symbol (cf-ch run k)) acc))))))))

(define (cf-tokenize s)
  (let ((n (string-length s)))
    (let loop ((i 0) (acc '()))
      (if (>= i n)
          (reverse acc)
          (let ((c (cf-ch s i)))
            (cond
              ((cf-space? c) (loop (+ i 1) acc))
              ((or (cf-digit? c)
                   (and (string=? c ".") (< (+ i 1) n) (cf-digit? (cf-ch s (+ i 1)))))
               (let* ((e1 (cf-scan s i n cf-digit?))
                      (ip (if (> e1 i) (string->number (substring s i e1)) 0)))
                 (if (and (< e1 n) (string=? (cf-ch s e1) "."))
                     (let* ((e2 (cf-scan s (+ e1 1) n cf-digit?))
                            (fv (if (> e2 (+ e1 1))
                                    (/ (string->number (substring s (+ e1 1) e2))
                                       (expt 10 (- e2 e1 1)))
                                    0)))
                       (loop e2 (cons (+ ip fv) acc)))
                     (loop e1 (cons ip acc)))))
              ((cf-alpha? c)
               (let* ((e1 (cf-scan s i n cf-alnum?))
                      (run (substring s i e1)))
                 (if (cf-has-digit-or-us? run)
                     (loop e1 (cons (string->symbol run) acc))
                     (loop e1 (cf-split-ident run acc)))))
              ((string=? c "*")
               (if (and (< (+ i 1) n) (string=? (cf-ch s (+ i 1)) "*"))
                   (loop (+ i 2) (cons "^" acc))
                   (loop (+ i 1) (cons "*" acc))))
              ((or (string=? c "+") (string=? c "-") (string=? c "/") (string=? c "^")
                   (string=? c "(") (string=? c ")") (string=? c ",") (string=? c "=")
                   (string=? c "[") (string=? c "]"))
               (loop (+ i 1) (cons c acc)))
              (else
               (cf-fail (string-append "unexpected character '" c "'"))
               (loop (+ i 1) acc))))))))

;;; --- 3. Recursive-descent parser ---

(define cf-toks '())

(define (cf-peek) (if (null? cf-toks) "" (car cf-toks)))

(define (cf-next!)
  (if (null? cf-toks)
      (cf-fail "unexpected end of input")
      (let ((t (car cf-toks)))
        (set! cf-toks (cdr cf-toks))
        t)))

(define (cf-op? t s) (and (string? t) (string=? t s)))

(define (cf-expect s)
  (if (cf-op? (cf-peek) s)
      (cf-next!)
      (cf-fail (string-append "expected '" s "'"))))

(define (cf-starts-primary? t)
  (or (number? t) (symbol? t) (cf-op? t "(") (cf-op? t "[")))

(define (cf-fn-symbol? t)
  (and (symbol? t) (member (symbol->string t) cf-fn-names)))

(define (cf-parse-args close)
  (if (cf-op? (cf-peek) close)
      (begin (cf-next!) '())
      (let loop ((acc (list (cf-parse-eq))))
        (cond ((cf-op? (cf-peek) ",")
               (cf-next!)
               (loop (cons (cf-parse-eq) acc)))
              (else
               (cf-expect close)
               (reverse acc))))))

(define (cf-parse-primary)
  (let ((t (cf-next!)))
    (cond ((number? t) t)
          ((symbol? t)
           (if (and (cf-fn-symbol? t) (cf-op? (cf-peek) "("))
               (begin (cf-next!)
                      (cons (if (eq? t 'ln) 'log t) (cf-parse-args ")")))
               t))
          ((cf-op? t "(")
           (let ((e (cf-parse-eq)))
             (cf-expect ")")
             e))
          ((cf-op? t "[")
           (cons 'cas-list (cf-parse-args "]")))
          (t (cf-fail (string-append "unexpected '" t "'")))
          (else #f))))

(define (cf-parse-power)
  (let ((b (cf-parse-primary)))
    (if (cf-op? (cf-peek) "^")
        (begin (cf-next!)
               (list '^ b (cf-parse-unary)))
        b)))

(define (cf-parse-unary)
  (cond ((cf-op? (cf-peek) "-")
         (cf-next!)
         (list '* -1 (cf-parse-unary)))
        ((cf-op? (cf-peek) "+")
         (cf-next!)
         (cf-parse-unary))
        (else (cf-parse-power))))

(define (cf-parse-term)
  (let loop ((acc (cf-parse-unary)))
    (cond ((cf-op? (cf-peek) "*")
           (cf-next!)
           (loop (list '* acc (cf-parse-unary))))
          ((cf-op? (cf-peek) "/")
           (cf-next!)
           (loop (list '/ acc (cf-parse-unary))))
          ((cf-starts-primary? (cf-peek))
           (loop (list '* acc (cf-parse-power))))
          (else acc))))

(define (cf-parse-expr)
  (let loop ((acc (cf-parse-term)))
    (cond ((cf-op? (cf-peek) "+")
           (cf-next!)
           (loop (list '+ acc (cf-parse-term))))
          ((cf-op? (cf-peek) "-")
           (cf-next!)
           (loop (list '- acc (cf-parse-term))))
          (else acc))))

(define (cf-parse-eq)
  (let ((l (cf-parse-expr)))
    (if (cf-op? (cf-peek) "=")
        (begin (cf-next!)
               (list '= l (cf-parse-expr)))
        l)))

;; (from-infix "3x^2 + 2x") -> prefix AST, or #f (with a message printed) on error
(define (from-infix s)
  (set! cf-err #f)
  (set! cf-toks (cf-tokenize s))
  (let ((ast (cf-parse-eq)))
    (if (and (not cf-err) (not (null? cf-toks)))
        (cf-fail (string-append "unexpected trailing input")))
    (if cf-err
        (begin (display "parse error: ") (display cf-err) (newline) #f)
        ast)))

;;; --- 4. Command evaluation: calc / math ---

(define cf-commands '(diff integrate expand simplify solve))

(define (cf-apply-cmd cmd args)
  (let ((real (cond ((and (eq? cmd 'integrate) (= (length args) 4)) 'integrate-def)
                    ((and (eq? cmd 'solve) (pair? args) (pair? (car args)) (pair? (caar args)))
                     'solve-system)
                    ((and (eq? cmd 'solve) (pair? args) (pair? (car args)) (eq? (caar args) '=)
                          (pair? (cdr args)) (pair? (cadr args)))
                     'solve-system)
                    (else cmd))))
    (eval (cons real (map (lambda (a) (list 'quote a)) args)))))

(define (cf-eval ast)
  (cond ((not (pair? ast)) ast)
        ((eq? (car ast) 'cas-list) (map cf-eval (cdr ast)))
        ((memq (car ast) cf-commands)
         (cf-apply-cmd (car ast) (map cf-eval (cdr ast))))
        (else (cons (car ast) (map cf-eval (cdr ast))))))

(define (calc s)
  (let ((ast (from-infix s)))
    (if (not ast)
        #f
        (let ((r (cf-eval ast)))
          (if (and (pair? ast) (or (memq (car ast) cf-commands) (eq? (car ast) 'cas-list)))
              r
              (simplify r))))))

(define (math s)
  (let ((r (calc s)))
    (if r
        (begin (display (to-infix r)) (newline) #t)
        #f)))
