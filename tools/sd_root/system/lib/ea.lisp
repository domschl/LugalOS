;;; /sd0/system/lib/ea.lisp — LugalOS Lisp Agent Engine
;;;
;;; Usage:
;;;   (load "/sd0/system/lib/ea.lisp")
;;;   (ea "What is the integral of x*cos(x)?")

(define (ea-prompt prompt . args)
  (let* ((model (if (null? args) #f (car args)))
         (sys-prompt "You are ea, an AI assistant running inside LugalOS.
You have access to symbolic mathematics via CAS libraries in /sd0/cas/.
Format for actions:
Action: lisp
```lisp
(load \"/sd0/cas/calculus.lisp\")
(diff '(sin x) 'x)
```
Answer: <final answer>"))
    (display "\n[ea] Querying model...\n")
    (llm-chat prompt sys-prompt model)))

(define (ea-ask prompt . args)
  (let ((ans (apply ea-prompt (cons prompt args))))
    (if (string? ans)
        (begin (display ans) (newline))
        (display "[ea] No response from server\n"))))
