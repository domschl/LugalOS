;;; LugalOS update state as data
;;; /sd0/system/lib/ota.lisp
;;;
;;;   (require 'ota)
;;;   (ota-info)  -> (("board" . "rp2350-wifi") ("running" . "a") ("slot_a" . "1064.df04efbc+") ...)
;;;   (ota-get "running")          -> "a"
;;;   (ota-get "at_a")             -> "0x400000+0x400000", the slot's place in flash
;;;
;;; The pairs are /dev/ota's lines (doc/admin/updates.md): board, chip, layout,
;;; version (boards that order images by one), running, trial, fell_back,
;;; slot_a, slot_b (build id or "empty"), at_a, at_b, at_flashfs, at_records,
;;; at_identity (and at_loader or at_table), active, pending, release_key,
;;; signatures, state. Read-only; a board with no update path gives
;;; (("supported" . "no")).

(require 'str)

;; Index of the first `ch` (a one-character string) in `str`, or #f.
(define (ota-index str ch)
  (let loop ((i 0))
    (cond ((>= i (string-length str)) #f)
          ((string=? (substring str i (+ i 1)) ch) i)
          (else (loop (+ i 1))))))

(define (ota-info)
  (let loop ((lines (string-split (read-file "/dev/ota") "\n")) (acc '()))
    (cond
      ((null? lines) (reverse acc))
      (else
       (let* ((line (car lines))
              (eq (ota-index line "=")))
         (if eq
             (loop (cdr lines)
                   (cons (cons (substring line 0 eq)
                               (substring line (+ eq 1) (string-length line)))
                         acc))
             (loop (cdr lines) acc)))))))

(define (ota-get key)
  (let ((pair (assoc key (ota-info))))
    (if pair (cdr pair) #f)))
