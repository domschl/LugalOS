;;; LugalOS Association & Property List Utilities
;;; /sd0/system/lib/dict.lisp

;; (alist-ref alist key [default])
(define (alist-ref alist key . opts)
  (let ((default (if (null? opts) #f (car opts)))
        (pair (assoc key alist)))
    (if pair
        (cdr pair)
        default)))

;; (alist-set alist key val) -> returns new alist with key updated or prepended
(define (alist-set alist key val)
  (let loop ((l alist) (found #f) (acc '()))
    (if (null? l)
        (if found
            (reverse acc)
            (cons (cons key val) alist))
        (let ((entry (car l)))
          (if (equal? (car entry) key)
              (loop (cdr l) #t (cons (cons key val) acc))
              (loop (cdr l) found (cons entry acc)))))))

;; (alist-delete alist key)
(define (alist-delete alist key)
  (let loop ((l alist) (acc '()))
    (if (null? l)
        (reverse acc)
        (if (equal? (car (car l)) key)
            (loop (cdr l) acc)
            (loop (cdr l) (cons (car l) acc))))))

;; (plist-get plist key [default])
;; e.g. (plist-get '(a 1 b 2) 'b) -> 2
(define (plist-get plist key . opts)
  (let ((default (if (null? opts) #f (car opts))))
    (let loop ((l plist))
      (cond ((null? l) default)
            ((null? (cdr l)) default)
            ((equal? (car l) key) (cadr l))
            (else (loop (cddr l)))))))

;; (plist-put plist key val)
(define (plist-put plist key val)
  (let loop ((l plist) (acc '()))
    (cond ((null? l)
           (append (reverse acc) (list key val)))
          ((null? (cdr l))
           (append (reverse acc) (list key val)))
          ((equal? (car l) key)
           (append (reverse acc) (list key val) (cddr l)))
          (else
           (loop (cddr l) (cons (cadr l) (cons (car l) acc)))))))
