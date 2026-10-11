;; LugalOS Scheme / Lisp Standard Library
;; /sd0/system/etc/stdlib.lisp
;;
;; Minimal Core Profile: loads at boot with near-zero node pool consumption.
;; Additional libraries (seq, str, dict, macros) can be loaded on demand via (require 'module).

(define not (lambda (x) (if x #f #t)))

(define print (lambda (msg)
  (begin
    (display msg)
    (newline))))

(define-macro (when test . body)
  `(if ,test (begin ,@body) #f))

(define-macro (unless test . body)
  `(if (not ,test) (begin ,@body) #f))

;; Module registry & on-demand loader for modular profiles
(define *modules* '())

(define (require mod)
  (if (member mod *modules*)
      #t
      (let* ((name (if (symbol? mod) (symbol->string mod) mod))
             (p1 (string-append "/sd0/system/lib/" name ".lisp"))
             (p2 (string-append "/flash0/system/lib/" name ".lisp"))
             (path (if (> (string-length (read-file p1)) 0) p1
                       (if (> (string-length (read-file p2)) 0) p2 #f))))
        (if path
            (begin
              (set! *modules* (cons mod *modules*))
              (load path)
              #t)
            (error "require: cannot find module " name)))))
