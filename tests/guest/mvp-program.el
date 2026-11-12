;;; mvp-program.el --- Program authored during guest acceptance -*- lexical-binding: t; -*-

(require 'emacsos-app)
(defvar mvp-counts (make-hash-table :test #'eql))
(defvar mvp-timers (make-hash-table :test #'eql))

(defun mvp-entry ()
  (let* ((id emacsos-app-current)
         (buffer (emacsos-app-buffer id (format "*MVP run %d*" id)))
         (tick (lambda ()
                 (if (not (buffer-live-p buffer))
                     (emacsos-app-finish id "Buffer closed")
                   (puthash id (1+ (gethash id mvp-counts 0)) mvp-counts)
                   (with-current-buffer buffer
                     (unless (buffer-modified-p)
                       (erase-buffer)
                       (insert (format "Run %d: %d\n" id (gethash id mvp-counts)))
                       (set-buffer-modified-p nil)))))))
    (funcall tick)
    (puthash id (emacsos-app-run-at-time id 1 1 tick) mvp-timers)
    (emacsos-app-set-wait-reason id "Counter timer")))

(defun mvp-start ()
  (emacsos-app-start "MVP counter" #'mvp-entry nil "/work/mvp-program.el"))

(provide 'mvp-program)
