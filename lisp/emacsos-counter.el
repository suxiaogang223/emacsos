;;; emacsos-counter.el --- Minimal independent application demo -*- lexical-binding: t; -*-

;; Copyright (C) 2026 emacsOS contributors
;; SPDX-License-Identifier: GPL-3.0-or-later

;;; Commentary:
;; Start twice with `emacsos-counter-start'; inspect with `emacsos-processes'.
;; Each invocation owns a timer and buffer.  User edits pause display updates
;; and preserve the buffer on stop.  There is no persistence or isolation.

;;; Code:

(require 'emacsos-app)

(defun emacsos-counter--entry ()
  (let* ((id emacsos-app-current)
         (buffer (emacsos-app-buffer id (format "*Counter %d*" id)))
         (count 0))
    (with-current-buffer buffer (buffer-disable-undo))
    (cl-labels
        ((tick ()
           (if (not (buffer-live-p buffer))
               (emacsos-app-finish id "Counter buffer closed")
             (cl-incf count)
             (with-current-buffer buffer
               (unless (buffer-modified-p)
                 (erase-buffer)
                 (insert (format "Counter run %d: %d\n" id count))
                 (set-buffer-modified-p nil))))))
      (tick)
      (emacsos-app-run-at-time id 1 1 #'tick))
    (emacsos-app-set-wait-reason id "Next counter tick")
    (emacsos-app-log id (format "Display buffer: %s" (buffer-name buffer)))))

(defun emacsos-counter-start ()
  "Start and display a new independent trusted counter instance; return its ID."
  (interactive)
  (let* ((source (or (locate-library "emacsos-counter.el") load-file-name))
         (id (emacsos-app-start "Counter" #'emacsos-counter--entry nil source)))
    (pop-to-buffer (get-buffer (format "*Counter %d*" id)))
    id))

(provide 'emacsos-counter)
;;; emacsos-counter.el ends here
