git remote set-url origin git@github.com:$(git remote get-url origin | sed -E 's#https://github.com/##; s#\.git$##').git

