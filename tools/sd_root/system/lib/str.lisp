;;; LugalOS String Utilities
;;; /sd0/system/lib/str.lisp

;; (string-split str delim) -> list of strings
;; e.g. (string-split "a,b,c" ",") -> ("a" "b" "c")
(define (string-split str delim)
  (let ((len (string-length str))
        (dlen (string-length delim)))
    (if (= len 0)
        '()
        (let loop ((start 0) (idx 0) (acc '()))
          (cond
            ((> (+ idx dlen) len)
             (reverse (cons (substring str start len) acc)))
            ((string=? (substring str idx (+ idx dlen)) delim)
             (loop (+ idx dlen) (+ idx dlen) (cons (substring str start idx) acc)))
            (else
             (loop start (+ idx 1) acc)))))))

;; (string-join str-list delim) -> string
(define (string-join str-list delim)
  (cond ((null? str-list) "")
        ((null? (cdr str-list)) (car str-list))
        (else
         (let loop ((rest (cdr str-list)) (acc (car str-list)))
           (if (null? rest)
               acc
               (loop (cdr rest) (string-append acc delim (car rest))))))))

;; (string-prefix? prefix str)
(define (string-prefix? prefix str)
  (let ((plen (string-length prefix))
        (slen (string-length str)))
    (if (> plen slen)
        #f
        (string=? (substring str 0 plen) prefix))))

;; (string-suffix? suffix str)
(define (string-suffix? suffix str)
  (let ((xlen (string-length suffix))
        (slen (string-length str)))
    (if (> xlen slen)
        #f
        (string=? (substring str (- slen xlen) slen) suffix))))

;; (string-contains? needle haystack)
(define (string-contains? needle haystack)
  (let ((nlen (string-length needle))
        (hlen (string-length haystack)))
    (if (> nlen hlen)
        #f
        (let loop ((i 0))
          (cond ((> (+ i nlen) hlen) #f)
                ((string=? (substring haystack i (+ i nlen)) needle) #t)
                (else (loop (+ i 1))))))))

;; (string-trim str)
(define (string-trim str)
  (let ((len (string-length str)))
    (if (= len 0)
        ""
        (let* ((start (let loop ((i 0))
                        (if (>= i len)
                            len
                            (let ((c (substring str i (+ i 1))))
                              (if (or (string=? c " ") (string=? c "\t") (string=? c "\n") (string=? c "\r"))
                                  (loop (+ i 1))
                                  i)))))
               (end (let loop ((i len))
                      (if (<= i start)
                          start
                          (let ((c (substring str (- i 1) i)))
                            (if (or (string=? c " ") (string=? c "\t") (string=? c "\n") (string=? c "\r"))
                                (loop (- i 1))
                                i))))))
          (substring str start end)))))
