;;; desktop.el --- Linux exploration entry point -*- lexical-binding: t; -*-

(setq inhibit-startup-screen t
      initial-scratch-message nil
      make-backup-files t
      create-lockfiles t)

(add-hook 'emacs-startup-hook
          (lambda ()
            (switch-to-buffer "*emacsOS*")
            (insert "Welcome to emacsOS on Linux.\n\n"
                    "C-x C-f  Open or create a file\n"
                    "C-x d    Browse files\n"
                    "M-x eshell  Open a command environment\n"
                    "M-x list-processes  Inspect background processes\n"
                    "C-x C-c  Exit and shut down\n\n"
                    "Your working directory is ~/work.\n")
            (goto-char (point-min))
            (message "EMACSOS-LINUX-READY")))

;;; desktop.el ends here
