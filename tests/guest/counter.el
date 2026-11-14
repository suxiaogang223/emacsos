;;; counter.el --- Counter authored during guest integration -*- lexical-binding: t; -*-

(require 'emacsos-app)
(defvar guest-counts (make-hash-table :test #'eql))
(defvar guest-timers (make-hash-table :test #'eql))

(defun guest-entry ()
  (let* ((id emacsos-app-current)
         (buffer (emacsos-app-buffer id (format "*Guest run %d*" id)))
         (tick (lambda ()
                 (if (not (buffer-live-p buffer))
                     (emacsos-app-finish id "Buffer closed")
                   (puthash id (1+ (gethash id guest-counts 0)) guest-counts)
                   (with-current-buffer buffer
                     (unless (buffer-modified-p)
                       (erase-buffer)
                       (insert (format "Run %d: %d\n" id (gethash id guest-counts)))
                       (set-buffer-modified-p nil)))))))
    (funcall tick)
    (puthash id (emacsos-app-run-at-time id 1 1 tick) guest-timers)
    (emacsos-app-set-wait-reason id "Counter timer")))

(defun guest-start ()
  (emacsos-app-start "Guest counter" #'guest-entry nil "/work/counter.el"))

(provide 'guest-counter)
