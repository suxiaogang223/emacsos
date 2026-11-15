;;; memory.el --- Allocation and GC regression in the actual guest -*- lexical-binding: t; -*-

;; Keeping roots alive across GC must not depend on identity-mapped backing.
(let ((root (list (make-string 524288 65) (make-vector 32768 42)))
      (before gcs-done)
      (gc-cons-threshold 1048576))
  (dotimes (_ 3)
    (let (garbage)
      (dotimes (_ 12)
        (push (cons (make-string 524288 90) (make-vector 4096 7)) garbage))
      (unless (= (length garbage) 12)
        (error "Allocation differs")))
    (garbage-collect))
  (unless (and (> gcs-done before)
               (= (length (car root)) 524288)
               (= (aref (car root) 524287) 65)
               (= (aref (cadr root) 32767) 42))
    (error "GC damaged live objects")))
