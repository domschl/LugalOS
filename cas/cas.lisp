;;; LugalOS CAS loader: dynamically detects whether CAS is mounted on /sd0 or /flash0
(define *cas-dir*
  (if (> (string-length (read-file "/sd0/cas/simplify.lisp")) 0)
      "/sd0/cas/"
      "/flash0/cas/"))

(load (string-append *cas-dir* "simplify.lisp"))
(load (string-append *cas-dir* "poly.lisp"))
(load (string-append *cas-dir* "calculus.lisp"))
(load (string-append *cas-dir* "solve.lisp"))
(load (string-append *cas-dir* "format.lisp"))
(load (string-append *cas-dir* "plot.lisp"))
