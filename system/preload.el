;;; preload.el --- Generate an explicit dump load order -*- lexical-binding: t; -*-

;; Probe dependencies in a normal build-tree Emacs. GNU Emacs requires explicit
;; loads while dumping, so replay the dependency completion order in site-init.
(let* ((root (expand-file-name "lisp/"))
       (loaded (mapcar (lambda (name)
                         (replace-regexp-in-string "\\.elc?\\'" "" name))
                       preloaded-file-list)))
  (dolist (library '(dired info find-func eshell esh-mode
                    em-alias em-banner em-basic em-cmpl em-dirs em-extpipe
                    em-glob em-hist em-ls em-pred em-prompt em-script
                    em-term em-unix org org-agenda))
    (require library))
  (with-temp-file (expand-file-name "site-init.el" root)
    (insert ";;; site-init.el --- Generated emacsOS library preload\n")
    ;; Portable dumps also retain ordinary heap objects. Some optional-library
    ;; constants (such as bool vectors) cannot be copied into Emacs pure space.
    ;; Org's mail-link default reads identity before startup initializes it.
    ;; Bind empty identities only while loading, then defer that default below.
    (insert "(let ((purify-flag nil) (user-mail-address nil) (user-full-name nil) (load-path (append '")
    (prin1 (delq nil (mapcar (lambda (directory)
                               (when (and directory (file-in-directory-p directory root))
                                 directory))
                             load-path))
           (current-buffer))
    (insert " load-path)))\n")
    (dolist (entry (reverse load-history))
      (let ((file (car entry)))
        (when (and (stringp file) (file-in-directory-p file root))
          (let ((name (replace-regexp-in-string
                       "\\.elc?\\'" "" (file-relative-name file root))))
            (unless (or (member name loaded)
                        (member name '("loadup" "loaddefs" "subdirs" "site-init")))
              (push name loaded)
              ;; calendar provides itself before requiring cal-menu upstream.
              ;; Replay that early provide to break their explicit-load cycle.
              (when (equal name "calendar/cal-menu")
                (insert "(provide 'calendar)\n"))
              (prin1 `(load ,name nil t) (current-buffer))
              (insert "\n"))))))
    ;; Org keeps detached scratch overlays. Portable dumps cannot contain
    ;; overlays; recreate these empty objects, with their properties, at startup.
    (dolist (variable '(org-tags-overlay org-date-ovl org-hl
                       org-speedbar-restriction-lock-overlay
                       org-agenda-restriction-lock-overlay))
      (let ((properties (overlay-properties (symbol-value variable))))
        (prin1 `(delete-overlay ,variable) (current-buffer))
        (insert "\n")
        (prin1 `(setq ,variable nil) (current-buffer))
        (insert "\n")
        (prin1 `(add-hook 'after-init-hook
                          (lambda ()
                            (let ((overlay (make-overlay 1 1))
                                  (properties ',properties))
                              (while properties
                                (overlay-put overlay (pop properties) (pop properties)))
                              (delete-overlay overlay)
                              (setq ,variable overlay))))
               (current-buffer))
        (insert "\n")))
    ;; Ordinary (non-pure) loads record build-tree paths. Keep source lookup
    ;; working after installing the matching libraries in the runtime image.
    (prin1 `(setq load-history
                  (mapcar (lambda (entry)
                            (let ((file (car entry)))
                              (if (and (stringp file) (string-prefix-p ,root file))
                                  (cons (concat ,(format "/usr/share/emacs/%s/lisp/" emacs-version)
                                                (substring file ,(length root)))
                                        (cdr entry))
                                entry)))
                          load-history))
           (current-buffer))
    (insert "\n")
    ;; Preserve user-dependent defaults until normal startup has its real HOME.
    (dolist (variable '(eshell-directory-name eshell-aliases-file
                       eshell-last-dir-ring-file-name eshell-history-file-name
                       eshell-login-script eshell-rc-script eshell-no-grep-available
                       org-link-from-user-regexp))
      (prin1 `(custom-initialize-delay ',variable
                                      (car (get ',variable 'standard-value)))
             (current-buffer))
      (insert "\n"))
    (insert ")\n")))

;;; preload.el ends here
