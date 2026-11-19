;;; desktop.el --- A text workspace for emacsOS -*- lexical-binding: t; -*-

(require 'button)
(defvar org-directory)
(defvar org-agenda-files)
(defvar org-modules)

(setq inhibit-startup-screen t
      initial-scratch-message nil
      frame-background-mode 'dark
      make-backup-files t
      create-lockfiles t)

(frame-set-background-mode (selected-frame))

(defvar emacsos-work-directory (expand-file-name "~/work/")
  "Directory for the user's ordinary working files.")

(defun emacsos--open-example (name contents)
  "Open NAME in the workspace, seeding CONTENTS only if it is absent.
An existing file or visiting buffer is never replaced.  New examples are
saved once, with exclusive creation; subsequent edits use normal Emacs saves."
  (let ((file (expand-file-name name emacsos-work-directory)))
    (unless (or (file-exists-p file) (get-file-buffer file))
      (with-temp-buffer
        (insert contents)
        (write-region (point-min) (point-max) file nil 'silent nil 'excl)))
    (find-file file)))

(defun emacsos-notes ()
  "Open the personal Org notebook using normal file editing commands."
  (interactive)
  (emacsos--open-example
   "notes.org"
   "#+title: Notebook\n\n* Notes\n** A computer I can change\nThis is an ordinary Org file. Edit it, then save with C-x C-s.\n\n- Follow a link with C-c C-o: [[info:emacs][The Emacs manual]]\n- Fold a heading with TAB.\n- Make a new heading with M-RET.\n\n** Ideas\nWhat would you like your computer to do for you?\n"))

(defun emacsos-tasks ()
  "Open the task file shared with the Org agenda."
  (interactive)
  (emacsos--open-example
   "tasks.org"
   (concat "#+title: Tasks\n\n* Tasks\n** TODO Write my first note\n   SCHEDULED: "
           (format-time-string "<%Y-%m-%d %a>")
           "\n   Open M-x emacsos-notes and make this workspace your own.\n\n** TODO Make a Lisp command\n   Try M-x emacsos-lisp, then M-x eval-buffer.\n\n* Someday\n** Explore the Emacs manual\n   M-x info opens the built-in documentation.\n")))

(defun emacsos-agenda ()
  "Open today's Org agenda for the workspace task file."
  (interactive)
  (require 'org-agenda)
  (emacsos-tasks)
  (org-agenda-list nil nil 1))

(defun emacsos-files ()
  "Browse the workspace with Dired."
  (interactive)
  (dired emacsos-work-directory))

(defun emacsos-shell ()
  "Open Eshell in the workspace, preserving an existing shell's directory."
  (interactive)
  (let ((default-directory emacsos-work-directory))
    (eshell)))

(defun emacsos-lisp ()
  "Open an editable Lisp example that defines an ordinary Emacs command."
  (interactive)
  (emacsos--open-example
   "hello.el"
   ";;; hello.el --- My first command -*- lexical-binding: t; -*-\n\n;; Evaluate with M-x eval-buffer, then run M-x my-hello.\n;; Change the message, save with C-x C-s, and evaluate again.\n\n(defun my-hello ()\n  \"Say hello from my computer.\"\n  (interactive)\n  (message \"Hello from my Lisp computer.\"))\n\n;; C-h f describes a function. C-x 2 splits the window.\n"))

(with-eval-after-load 'org
  ;; Keep first use focused on the offline workspace. Other standard Org
  ;; integrations remain installed and can be enabled through org-modules.
  (setq org-modules '(ol-info)
        org-directory emacsos-work-directory
        org-agenda-files (list (expand-file-name "tasks.org" emacsos-work-directory))))

(defvar emacsos-home-mode-map
  (let ((map (make-sparse-keymap)))
    (set-keymap-parent map special-mode-map)
    (define-key map (kbd "TAB") #'forward-button)
    (define-key map (kbd "<backtab>") #'backward-button)
    (define-key map (kbd "f") #'emacsos-files)
    (define-key map (kbd "n") #'emacsos-notes)
    (define-key map (kbd "a") #'emacsos-agenda)
    (define-key map (kbd "s") #'emacsos-shell)
    (define-key map (kbd "l") #'emacsos-lisp)
    (define-key map (kbd "h") #'info)
    (define-key map (kbd "g") #'emacsos-home)
    map)
  "Local shortcuts in the home buffer; standard Emacs bindings remain available.")

(define-derived-mode emacsos-home-mode special-mode "emacsOS"
  "Read-only home buffer with ordinary text buttons and local shortcuts."
  (setq-local truncate-lines t))

(defun emacsos-home ()
  "Return to the emacsOS home buffer without closing other buffers or windows."
  (interactive)
  (switch-to-buffer (get-buffer-create "*emacsOS*"))
  (emacsos-home-mode)
  (let ((inhibit-read-only t))
    (erase-buffer)
    (insert (propertize "emacsOS   Your machine. Your Lisp.\n" 'face 'bold)
            "\nYour working directory is ~/work.\n\n")
    (dolist (entry '(("f" "Files" emacsos-files "Browse the workspace in Dired")
                     ("n" "Notes" emacsos-notes "Write and link plain-text notes")
                     ("a" "Agenda" emacsos-agenda "Plan today with Org")
                     ("s" "Eshell" emacsos-shell "Work with commands and Lisp")
                     ("l" "Lisp" emacsos-lisp "Make a command of your own")
                     ("h" "Manual" info "Read the Emacs documentation")))
      (insert "  ")
      (insert-text-button (format "[%s] %-8s" (nth 0 entry) (nth 1 entry))
                          'follow-link t
                          'action (let ((command (nth 2 entry)))
                                    (lambda (_button) (call-interactively command))))
      (insert "  " (nth 3 entry) "\n"))
    (insert "\nTAB / RET  Select an entry     M-x       Run any command\n"
            "C-x b      Switch buffers      C-x 2     Split this window\n"
            "C-x C-f    Open a file          C-x C-s   Save your edits\n"
            "C-c o      Return here         C-x C-c   Exit and shut down\n"
            "\nBrowser files last for this session; restart discards them.\n")
    (set-buffer-modified-p nil)
    (goto-char (point-min))
    (forward-button 1)))

(global-set-key (kbd "C-c o") #'emacsos-home)

(add-hook 'emacs-startup-hook
          (lambda ()
            (emacsos-home)
            (redisplay t)
            (message "Your workspace is ready.")))

(provide 'emacsos)
;;; desktop.el ends here
